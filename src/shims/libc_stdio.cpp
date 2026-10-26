// <stdio.h>, file descriptors, and the file system calls, all routed through the Vfs.
//
// Every open file is an OpenFile in one table, addressed by its guest fd. A guest FILE* is
// a 152-byte guest block (sizeof(FILE) on bionic) that the table maps back to an fd; the
// game never looks inside one. stdin/stdout/stderr are the three FILEs of __sF.

#include "core/log.h"
#include "shims/common.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <unordered_map>

namespace fs = std::filesystem;

namespace rfu {

namespace {

std::int64_t host_tell(std::FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

int host_seek(std::FILE* f, std::int64_t off, int whence) {
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, off, whence);
#endif
}

struct OpenFile {
    std::FILE* fp = nullptr;      // a host file, or the host's stdin/stdout/stderr
    bool console = false;         // fp belongs to the host; never close it
    bool memory = false;          // a generated file (/proc/self/maps): data + pos
    std::string data;
    std::int64_t pos = 0;
    bool eof = false, error = false;
    std::string guest_path;
    bool directory = false;
    enum class Device { None, Null, Random } device = Device::None;
    std::shared_ptr<GuestSocket> socket;  // set for fds made by socket() and accept()

    ~OpenFile() {
        if (fp && !console) std::fclose(fp);
    }

    std::int64_t read(void* buf, std::uint64_t n) {
        if (device == Device::Null) return 0;
        if (device == Device::Random) {
            static std::mt19937_64 rng{std::random_device{}()};
            static std::mutex lock;
            std::lock_guard guard(lock);
            for (std::uint64_t i = 0; i < n; ++i) static_cast<std::uint8_t*>(buf)[i] = static_cast<std::uint8_t>(rng());
            return static_cast<std::int64_t>(n);
        }
        if (memory) {
            auto avail = static_cast<std::uint64_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(data.size()) - pos));
            n = std::min(n, avail);
            std::memcpy(buf, data.data() + pos, n);
            pos += static_cast<std::int64_t>(n);
            if (n == 0) eof = true;
            return static_cast<std::int64_t>(n);
        }
        if (!fp) return -1;
        std::size_t got = std::fread(buf, 1, n, fp);
        if (got < n) {
            if (std::feof(fp)) eof = true;
            if (std::ferror(fp)) error = true;
        }
        return static_cast<std::int64_t>(got);
    }

    std::int64_t write(const void* buf, std::uint64_t n) {
        if (device != Device::None) return static_cast<std::int64_t>(n);
        if (memory || !fp) return -1;
        std::size_t put = std::fwrite(buf, 1, n, fp);
        if (console) std::fflush(fp);
        if (put < n) error = true;
        return static_cast<std::int64_t>(put);
    }

    std::int64_t seek(std::int64_t off, int whence) {
        eof = false;
        if (memory) {
            std::int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? pos : static_cast<std::int64_t>(data.size());
            if (base + off < 0) return -1;
            pos = base + off;
            return pos;
        }
        if (!fp || console || host_seek(fp, off, whence) != 0) return -1;
        return host_tell(fp);
    }

    std::int64_t tell() { return memory ? pos : fp && !console ? host_tell(fp) : 0; }
};

struct Files {
    std::mutex mutex;
    std::map<int, std::shared_ptr<OpenFile>> fds;
    std::unordered_map<GuestAddr, int> streams;  // FILE* -> fd
    std::unordered_map<GuestAddr, std::pair<std::vector<std::string>, std::size_t>> dirs;  // DIR* -> entries, next
    GuestAddr sF = 0;

    Files() {
        for (int i = 0; i < 3; ++i) {
            auto f = std::make_shared<OpenFile>();
            f->fp = i == 0 ? stdin : i == 1 ? stdout : stderr;
            f->console = true;
            fds[i] = f;
        }
    }

    int add(std::shared_ptr<OpenFile> f) {
        std::lock_guard lock(mutex);
        int fd = 3;
        while (fds.count(fd)) ++fd;
        fds[fd] = std::move(f);
        return fd;
    }
    std::shared_ptr<OpenFile> get(int fd) {
        std::lock_guard lock(mutex);
        auto it = fds.find(fd);
        return it == fds.end() ? nullptr : it->second;
    }
    int fd_of(GuestAddr stream) {
        std::lock_guard lock(mutex);
        auto it = streams.find(stream);
        return it == streams.end() ? -1 : it->second;
    }
};

Files& files(Runtime& rt) { return rt.state<Files>(); }

int map_errno(int host) {
    switch (host) {
    case ENOENT: return android::kENOENT;
    case EEXIST: return android::kEEXIST;
    case EACCES: return android::kEACCES;
    case ENOTDIR: return android::kENOTDIR;
    case EISDIR: return android::kEISDIR;
    case ENOTEMPTY: return android::kENOTEMPTY;
    case ENOSPC: return android::kENOSPC;
    case EBUSY: return android::kEBUSY;
    default: return android::kEIO;
    }
}

int map_errno(const std::error_code& ec) { return map_errno(ec.value()); }

std::string guest_path(const CallContext& c, int i) { return c.rt().vfs().absolute(c.str(i)); }

// Opens a guest path. `flags` are Android open() flags. Returns the new fd or -errno.
int open_path(Runtime& rt, const std::string& path, int flags) {
    auto f = std::make_shared<OpenFile>();
    f->guest_path = path;
    if (path == "/dev/null" || path == "/dev/urandom" || path == "/dev/random") {
        f->device = path == "/dev/null" ? OpenFile::Device::Null : OpenFile::Device::Random;
        return files(rt).add(std::move(f));
    }
    if (auto gen = rt.vfs().virtual_file(path)) {
        if ((flags & android::kO_ACCMODE) != android::kO_RDONLY) return -android::kEACCES;
        f->memory = true;
        f->data = (*gen)();
        return files(rt).add(std::move(f));
    }
    auto host = rt.vfs().to_host(path);
    if (!host) return -android::kENOENT;
    std::error_code ec;
    bool exists = fs::exists(*host, ec);
    if (exists && fs::is_directory(*host, ec)) {
        if ((flags & android::kO_ACCMODE) != android::kO_RDONLY) return -android::kEISDIR;
        f->directory = true;
        return files(rt).add(std::move(f));
    }
    if ((flags & android::kO_DIRECTORY)) return -android::kENOTDIR;
    if (exists && (flags & android::kO_CREAT) && (flags & android::kO_EXCL)) return -android::kEEXIST;
    if (!exists && !(flags & android::kO_CREAT)) return -android::kENOENT;

    int acc = flags & android::kO_ACCMODE;
    const char* mode;
    if (acc == android::kO_RDONLY) mode = "rb";
    else if (flags & android::kO_APPEND) mode = acc == android::kO_WRONLY ? "ab" : "a+b";
    else if ((flags & android::kO_TRUNC) || !exists) mode = acc == android::kO_WRONLY ? "wb" : "w+b";
    else mode = "r+b";
#ifdef _WIN32
    f->fp = _wfopen(host->c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    f->fp = std::fopen(host->c_str(), mode);
#endif
    if (!f->fp) return -map_errno(errno);
    return files(rt).add(std::move(f));
}

int fopen_flags(const char* mode) {
    int flags = 0;
    bool plus = std::strchr(mode, '+') != nullptr;
    switch (mode[0]) {
    case 'r': flags = plus ? android::kO_RDWR : android::kO_RDONLY; break;
    case 'w': flags = (plus ? android::kO_RDWR : android::kO_WRONLY) | android::kO_CREAT | android::kO_TRUNC; break;
    case 'a': flags = (plus ? android::kO_RDWR : android::kO_WRONLY) | android::kO_CREAT | android::kO_APPEND; break;
    default: return -1;
    }
    if (std::strchr(mode, 'x')) flags |= android::kO_EXCL;
    return flags;
}

GuestAddr new_stream(Runtime& rt, int fd) {
    GuestAddr fp = rt.heap().alloc_zeroed(android::kSizeofFile);
    auto& f = files(rt);
    std::lock_guard lock(f.mutex);
    f.streams[fp] = fd;
    return fp;
}

std::shared_ptr<OpenFile> stream(CallContext& c, int arg) {
    int fd = files(c.rt()).fd_of(c.x(arg));
    return fd < 0 ? nullptr : files(c.rt()).get(fd);
}

void close_fd(Runtime& rt, int fd) {
    auto& f = files(rt);
    std::lock_guard lock(f.mutex);
    f.fds.erase(fd);
}

std::int64_t to_unix_seconds(fs::file_time_type t) {
    auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

// Fills a guest struct stat. Returns 0 or -errno.
int fill_stat(Runtime& rt, const std::string& path, GuestAddr st) {
    using namespace android::stat_off;
    auto& mem = rt.mem();
    std::memset(mem.host(st), 0, sizeof_stat);
    std::uint32_t mode;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (auto gen = rt.vfs().virtual_file(path)) {
        mode = android::kS_IFREG | 0444;
        size = (*gen)().size();
    } else {
        auto host = rt.vfs().to_host(path);
        if (!host) return -android::kENOENT;
        std::error_code ec;
        auto status = fs::status(*host, ec);
        if (ec || !fs::exists(status)) return -android::kENOENT;
        if (fs::is_directory(status)) {
            mode = android::kS_IFDIR | 0771;
        } else {
            mode = android::kS_IFREG | 0660;
            size = fs::file_size(*host, ec);
        }
        auto t = fs::last_write_time(*host, ec);
        if (!ec) mtime = to_unix_seconds(t);
    }
    mem.write<std::uint64_t>(st + ino, std::hash<std::string>{}(path));
    mem.write<std::uint32_t>(st + android::stat_off::mode, mode);
    mem.write<std::uint32_t>(st + nlink, 1);
    mem.write<std::uint32_t>(st + uid, 10000);
    mem.write<std::uint32_t>(st + gid, 10000);
    mem.write<std::int64_t>(st + android::stat_off::size, static_cast<std::int64_t>(size));
    mem.write<std::int32_t>(st + blksize, 4096);
    mem.write<std::int64_t>(st + blocks, static_cast<std::int64_t>((size + 511) / 512));
    mem.write<std::int64_t>(st + atime, mtime);
    mem.write<std::int64_t>(st + android::stat_off::mtime, mtime);
    mem.write<std::int64_t>(st + ctime, mtime);
    return 0;
}

// printf-family output to a stream; returns the byte count or -1.
std::int64_t print_to(CallContext& c, int stream_arg, const std::string& text) {
    auto f = stream(c, stream_arg);
    if (!f) return -1;
    return f->write(text.data(), text.size()) == static_cast<std::int64_t>(text.size()) ? static_cast<std::int64_t>(text.size()) : -1;
}

void ret_int(CallContext& c, std::int64_t v) { c.ret(static_cast<std::uint64_t>(v)); }

// snprintf-style copy of `text` into a guest buffer of `size` bytes.
void store_truncated(CallContext& c, GuestAddr buf, std::uint64_t size, const std::string& text) {
    if (!size || !buf) return;
    std::size_t n = std::min<std::size_t>(text.size(), size - 1);
    std::memcpy(c.mem().host(buf), text.data(), n);
    c.mem().host(buf)[n] = 0;
}

// swprintf: the format is converted to UTF-8, formatted, and converted back.
void wide_print(CallContext& c, VarArgs& args) {
    GuestAddr buf = c.x(0);
    std::uint64_t n = c.x(1);
    std::u32string fmt = c.mem().read_wstr(c.x(2));
    std::u32string out = utf8_to_u32(guest_format(c.mem(), u32_to_utf8(fmt).c_str(), args));
    if (n == 0) return ret_int(c, -1);
    std::size_t len = std::min<std::size_t>(out.size(), n - 1);
    c.mem().write_bytes(buf, out.data(), len * 4);
    c.mem().write<std::uint32_t>(buf + len * 4, 0);
    ret_int(c, out.size() < n ? static_cast<std::int64_t>(out.size()) : -1);
}

}  // namespace

int add_socket_fd(Runtime& rt, std::shared_ptr<GuestSocket> s) {
    auto f = std::make_shared<OpenFile>();
    f->socket = std::move(s);
    return files(rt).add(std::move(f));
}

std::shared_ptr<GuestSocket> socket_at(Runtime& rt, int fd) {
    auto f = files(rt).get(fd);
    return f ? f->socket : nullptr;
}

bool fd_is_open(Runtime& rt, int fd) { return files(rt).get(fd) != nullptr; }

std::int64_t guest_pread(Runtime& rt, int fd, void* buf, std::uint64_t n, std::int64_t offset) {
    auto f = files(rt).get(fd);
    if (!f) return -1;
    std::int64_t saved = f->tell();
    if (f->seek(offset, SEEK_SET) < 0) return -1;
    std::int64_t got = f->read(buf, n);
    f->seek(saved, SEEK_SET);
    return got;
}

void register_libc_stdio(ShimRegistry& reg) {
    reg.add_data("__sF", {3 * android::kSizeofFile, [](Runtime& rt, GuestAddr addr) {
                              auto& f = files(rt);
                              std::lock_guard lock(f.mutex);
                              f.sF = addr;
                              for (int i = 0; i < 3; ++i) f.streams[addr + android::kSizeofFile * static_cast<unsigned>(i)] = i;
                          }});

    // --- fd level ------------------------------------------------------------------------
    auto open = [](CallContext& c) {
        int fd = open_path(c.rt(), guest_path(c, 0), static_cast<int>(c.x(1)));
        log::debug("open({}) = {}", c.str(0), fd);
        if (fd < 0) return c.fail(-fd);
        c.ret(static_cast<std::uint64_t>(fd));
    };
    reg.add("open", open);
    reg.add("__open_2", open);
    reg.add("close", [](CallContext& c) {
        int fd = static_cast<int>(c.x(0));
        if (fd < 3) return c.ret(0);
        if (!files(c.rt()).get(fd)) return c.fail(android::kEBADF);
        close_fd(c.rt(), fd);
        c.ret(0);
    });
    reg.add("read", [](CallContext& c) {
        auto f = files(c.rt()).get(static_cast<int>(c.x(0)));
        if (!f) return c.fail(android::kEBADF);
        if (f->socket) {
            std::int64_t n = socket_read(*f->socket, c.ptr(1), c.x(2));
            return n < 0 ? c.fail(static_cast<int>(-n)) : ret_int(c, n);
        }
        std::int64_t n = f->read(c.ptr(1), c.x(2));
        n < 0 ? c.fail(android::kEIO) : ret_int(c, n);
    });
    reg.add("write", [](CallContext& c) {
        auto f = files(c.rt()).get(static_cast<int>(c.x(0)));
        if (!f) return c.fail(android::kEBADF);
        if (f->socket) {
            std::int64_t n = socket_write(*f->socket, c.ptr(1), c.x(2));
            return n < 0 ? c.fail(static_cast<int>(-n)) : ret_int(c, n);
        }
        std::int64_t n = f->write(c.ptr(1), c.x(2));
        n < 0 ? c.fail(android::kEBADF) : ret_int(c, n);
    });
    reg.add("lseek", [](CallContext& c) {
        auto f = files(c.rt()).get(static_cast<int>(c.x(0)));
        if (!f) return c.fail(android::kEBADF);
        std::int64_t pos = f->seek(static_cast<std::int64_t>(c.x(1)), static_cast<int>(c.x(2)));
        pos < 0 ? c.fail(android::kESPIPE) : ret_int(c, pos);
    });
    reg.add("fsync", [](CallContext& c) {
        if (auto f = files(c.rt()).get(static_cast<int>(c.x(0))); f && f->fp) std::fflush(f->fp);
        c.ret(0);
    });
    reg.add("fstat", [](CallContext& c) {
        auto f = files(c.rt()).get(static_cast<int>(c.x(0)));
        if (!f) return c.fail(android::kEBADF);
        if (f->console) {
            std::memset(c.mem().host(c.x(1)), 0, android::stat_off::sizeof_stat);
            c.mem().write<std::uint32_t>(c.x(1) + android::stat_off::mode, 0020666);  // character device
            return c.ret(0);
        }
        if (f->fp) std::fflush(f->fp);
        int r = fill_stat(c.rt(), f->guest_path, c.x(1));
        r < 0 ? c.fail(-r) : c.ret(0);
    });
    auto stat = [](CallContext& c) {
        int r = fill_stat(c.rt(), guest_path(c, 0), c.x(1));
        r < 0 ? c.fail(-r) : c.ret(0);
    };
    reg.add("stat", stat);
    reg.add("lstat", stat);
    reg.add("access", [](CallContext& c) {
        std::string path = guest_path(c, 0);
        if (c.rt().vfs().virtual_file(path)) return c.ret(0);
        auto host = c.rt().vfs().to_host(path);
        std::error_code ec;
        if (!host || !fs::exists(*host, ec)) return c.fail(android::kENOENT);
        c.ret(0);
    });
    reg.add("mkdir", [](CallContext& c) {
        auto host = c.rt().vfs().to_host(guest_path(c, 0));
        if (!host) return c.fail(android::kEACCES);
        std::error_code ec;
        if (fs::exists(*host, ec)) return c.fail(android::kEEXIST);
        if (!fs::create_directory(*host, ec)) return c.fail(map_errno(ec));
        c.ret(0);
    });
    reg.add("rmdir", [](CallContext& c) {
        auto host = c.rt().vfs().to_host(guest_path(c, 0));
        std::error_code ec;
        if (!host || !fs::is_directory(*host, ec)) return c.fail(android::kENOTDIR);
        if (!fs::remove(*host, ec)) return c.fail(map_errno(ec));
        c.ret(0);
    });
    auto unlink = [](CallContext& c) {
        auto host = c.rt().vfs().to_host(guest_path(c, 0));
        std::error_code ec;
        if (!host || !fs::exists(*host, ec)) return c.fail(android::kENOENT);
        if (!fs::remove(*host, ec)) return c.fail(map_errno(ec));
        c.ret(0);
    };
    reg.add("unlink", unlink);
    reg.add("remove", unlink);
    reg.add("rename", [](CallContext& c) {
        auto from = c.rt().vfs().to_host(guest_path(c, 0));
        auto to = c.rt().vfs().to_host(guest_path(c, 1));
        if (!from || !to) return c.fail(android::kENOENT);
        std::error_code ec;
        fs::rename(*from, *to, ec);
        ec ? c.fail(map_errno(ec)) : c.ret(0);
    });
    reg.add("truncate", [](CallContext& c) {
        auto host = c.rt().vfs().to_host(guest_path(c, 0));
        std::error_code ec;
        if (!host) return c.fail(android::kENOENT);
        fs::resize_file(*host, c.x(1), ec);
        ec ? c.fail(map_errno(ec)) : c.ret(0);
    });
    reg.add("ftruncate", [](CallContext& c) {
        auto f = files(c.rt()).get(static_cast<int>(c.x(0)));
        if (!f || !f->fp) return c.fail(android::kEBADF);
        std::fflush(f->fp);
        auto host = c.rt().vfs().to_host(f->guest_path);
        std::error_code ec;
        if (host) fs::resize_file(*host, c.x(1), ec);
        ec ? c.fail(map_errno(ec)) : c.ret(0);
    });
    reg.add("getcwd", [](CallContext& c) {
        const std::string& cwd = c.rt().vfs().cwd();
        if (!c.x(0)) return c.ret(c.rt().heap().strdup(cwd));
        if (c.x(1) <= cwd.size()) return c.fail(android::kERANGE, 0);
        std::memcpy(c.str(0), cwd.c_str(), cwd.size() + 1);
        c.ret(c.x(0));
    });
    reg.add("chdir", [](CallContext& c) {
        c.rt().vfs().set_cwd(c.str(0));
        c.ret(0);
    });
    reg.add("realpath", [](CallContext& c) {
        std::string path = guest_path(c, 0);
        if (!c.x(1)) return c.ret(c.rt().heap().strdup(path));
        std::memcpy(c.str(1), path.c_str(), std::min<std::size_t>(path.size() + 1, 4096));
        c.ret(c.x(1));
    });
    reg.add("readlink", [](CallContext& c) { c.fail(android::kEINVAL); });
    for (const char* name : {"chmod", "fchmod", "fchmodat", "lchown", "utime", "utimensat"}) reg.add(name, [](CallContext& c) { c.ret(0); });
    for (const char* name : {"link", "symlink", "mkfifo", "mknod", "pipe", "dup2", "sendfile"}) reg.add(name, [](CallContext& c) { c.fail(android::kENOSYS); });
    reg.add("pathconf", [](CallContext& c) { c.ret(255); });
    reg.add("fcntl", [](CallContext& c) {
        if (auto s = socket_at(c.rt(), static_cast<int>(c.x(0)))) {
            int r = socket_fcntl(*s, static_cast<int>(c.x(1)), c.x(2));
            return r < 0 ? c.fail(-r) : c.ret(static_cast<std::uint64_t>(r));
        }
        c.ret(0);
    });
    reg.add("ioctl", [](CallContext& c) {
        if (auto s = socket_at(c.rt(), static_cast<int>(c.x(0)))) {
            int r = socket_ioctl(c, *s, c.x(1), c.x(2));
            return r < 0 ? c.fail(-r) : c.ret(static_cast<std::uint64_t>(r));
        }
        c.fail(25 /* ENOTTY */);
    });
    reg.add("statvfs", [](CallContext& c) {
        GuestAddr b = c.x(1);
        std::memset(c.mem().host(b), 0, 112);
        for (int off : {0, 8}) c.mem().write<std::uint64_t>(b + off, 4096);           // f_bsize, f_frsize
        for (int off : {16, 24, 32}) c.mem().write<std::uint64_t>(b + off, 1ull << 24);  // blocks, bfree, bavail
        c.mem().write<std::uint64_t>(b + 80, 255);                                     // f_namemax
        c.ret(0);
    });
    reg.add("statfs", [](CallContext& c) {
        GuestAddr b = c.x(1);
        std::memset(c.mem().host(b), 0, 120);
        c.mem().write<std::uint64_t>(b + 8, 4096);
        for (int off : {16, 24, 32}) c.mem().write<std::uint64_t>(b + off, 1ull << 24);
        c.mem().write<std::uint64_t>(b + 64, 255);
        c.mem().write<std::uint64_t>(b + 72, 4096);
        c.ret(0);
    });

    // --- directories -----------------------------------------------------------------------
    reg.add("opendir", [](CallContext& c) {
        auto host = c.rt().vfs().to_host(guest_path(c, 0));
        std::error_code ec;
        if (!host || !fs::is_directory(*host, ec)) return c.fail(android::kENOENT, 0);
        std::vector<std::string> names{".", ".."};
        for (const auto& e : fs::directory_iterator(*host, ec)) names.push_back(path_to_utf8(e.path().filename()));
        GuestAddr dir = c.rt().heap().alloc_zeroed(280);  // holds the struct dirent readdir returns
        auto& f = files(c.rt());
        std::lock_guard lock(f.mutex);
        f.dirs[dir] = {std::move(names), 0};
        c.ret(dir);
    });
    reg.add("readdir", [](CallContext& c) {
        auto& f = files(c.rt());
        std::lock_guard lock(f.mutex);
        auto it = f.dirs.find(c.x(0));
        if (it == f.dirs.end()) return c.fail(android::kEBADF, 0);
        auto& [names, next] = it->second;
        if (next >= names.size()) return c.ret(0);
        // struct dirent { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256]; }
        GuestAddr d = c.x(0);
        const std::string& name = names[next++];
        std::memset(c.mem().host(d), 0, 280);
        c.mem().write<std::uint64_t>(d, next);
        c.mem().write<std::int64_t>(d + 8, static_cast<std::int64_t>(next));
        c.mem().write<std::uint16_t>(d + 16, 280);
        c.mem().write<std::uint8_t>(d + 18, 0);  // DT_UNKNOWN: callers stat() if they care
        std::memcpy(c.mem().host(d + 19), name.c_str(), std::min<std::size_t>(name.size(), 255));
        c.ret(d);
    });
    reg.add("closedir", [](CallContext& c) {
        auto& f = files(c.rt());
        {
            std::lock_guard lock(f.mutex);
            if (!f.dirs.erase(c.x(0))) return c.fail(android::kEBADF);
        }
        c.rt().heap().free(c.x(0));
        c.ret(0);
    });

    // --- FILE* ---------------------------------------------------------------------------
    reg.add("fopen", [](CallContext& c) {
        int flags = fopen_flags(c.str(1));
        if (flags < 0) return c.fail(android::kEINVAL, 0);
        int fd = open_path(c.rt(), guest_path(c, 0), flags);
        log::debug("fopen({}, {}) = {}", c.str(0), c.str(1), fd);
        if (fd < 0) return c.fail(-fd, 0);
        if (c.str(1)[0] == 'a') files(c.rt()).get(fd)->seek(0, SEEK_END);
        c.ret(new_stream(c.rt(), fd));
    });
    reg.add("fdopen", [](CallContext& c) {
        int fd = static_cast<int>(c.x(0));
        if (!files(c.rt()).get(fd)) return c.fail(android::kEBADF, 0);
        c.ret(new_stream(c.rt(), fd));
    });
    reg.add("fclose", [](CallContext& c) {
        auto& f = files(c.rt());
        int fd;
        {
            std::lock_guard lock(f.mutex);
            auto it = f.streams.find(c.x(0));
            if (it == f.streams.end()) return c.fail(android::kEBADF, static_cast<std::uint64_t>(-1));
            fd = it->second;
            if (fd < 3) return c.ret(0);
            f.streams.erase(it);
        }
        close_fd(c.rt(), fd);
        c.rt().heap().free(c.x(0));
        c.ret(0);
    });
    reg.add("fileno", [](CallContext& c) { ret_int(c, files(c.rt()).fd_of(c.x(0))); });
    reg.add("fread", [](CallContext& c) {
        auto f = stream(c, 3);
        std::uint64_t size = c.x(1), n = c.x(2);
        if (!f || !size || !n) return c.ret(0);
        std::int64_t got = f->read(c.ptr(0), size * n);
        c.ret(got <= 0 ? 0 : static_cast<std::uint64_t>(got) / size);
    });
    reg.add("fwrite", [](CallContext& c) {
        auto f = stream(c, 3);
        std::uint64_t size = c.x(1), n = c.x(2);
        if (!f || !size || !n) return c.ret(0);
        std::int64_t put = f->write(c.ptr(0), size * n);
        c.ret(put <= 0 ? 0 : static_cast<std::uint64_t>(put) / size);
    });
    auto fgetc = [](CallContext& c) {
        auto f = stream(c, 0);
        unsigned char ch;
        if (!f || f->read(&ch, 1) != 1) return ret_int(c, -1);
        c.ret(ch);
    };
    reg.add("fgetc", fgetc);
    reg.add("getc", fgetc);
    reg.add("ungetc", [](CallContext& c) {
        auto f = stream(c, 1);
        if (!f || static_cast<std::int32_t>(c.x(0)) == -1 || f->seek(-1, SEEK_CUR) < 0) return ret_int(c, -1);
        c.ret(c.x(0) & 0xFF);
    });
    reg.add("fgets", [](CallContext& c) {
        auto f = stream(c, 2);
        auto n = static_cast<std::int32_t>(c.x(1));
        if (!f || n <= 0) return c.ret(0);
        char* buf = c.str(0);
        std::int32_t i = 0;
        while (i < n - 1) {
            char ch;
            if (f->read(&ch, 1) != 1) break;
            buf[i++] = ch;
            if (ch == '\n') break;
        }
        if (i == 0) return c.ret(0);
        buf[i] = 0;
        c.ret(c.x(0));
    });
    reg.add("fputc", [](CallContext& c) {
        auto f = stream(c, 1);
        char ch = static_cast<char>(c.x(0));
        if (!f || f->write(&ch, 1) != 1) return ret_int(c, -1);
        c.ret(c.x(0) & 0xFF);
    });
    reg.add("fputs", [](CallContext& c) { ret_int(c, print_to(c, 1, c.str(0)) < 0 ? -1 : 0); });
    reg.add("puts", [](CallContext& c) {
        std::string s = std::string(c.str(0)) + "\n";
        std::fwrite(s.data(), 1, s.size(), stdout);
        c.ret(0);
    });
    reg.add("putchar", [](CallContext& c) {
        std::fputc(static_cast<int>(c.x(0) & 0xFF), stdout);
        c.ret(c.x(0) & 0xFF);
    });
    reg.add("perror", [](CallContext& c) {
        int err = c.mem().read<std::int32_t>(c.thread().errno_addr);
        log::warn("perror: {}: {}", c.x(0) ? c.str(0) : "", android_strerror(err));
    });
    auto fseek = [](CallContext& c) {
        auto f = stream(c, 0);
        if (!f || f->seek(static_cast<std::int64_t>(c.x(1)), static_cast<int>(c.x(2))) < 0) return c.fail(android::kESPIPE);
        c.ret(0);
    };
    reg.add("fseek", fseek);
    reg.add("fseeko", fseek);
    auto ftell = [](CallContext& c) {
        auto f = stream(c, 0);
        if (!f) return c.fail(android::kEBADF);
        ret_int(c, f->tell());
    };
    reg.add("ftell", ftell);
    reg.add("ftello", ftell);
    reg.add("fsetpos", [](CallContext& c) {
        auto f = stream(c, 0);
        if (!f || f->seek(c.mem().read<std::int64_t>(c.x(1)), SEEK_SET) < 0) return c.fail(android::kEINVAL);
        c.ret(0);
    });
    reg.add("fflush", [](CallContext& c) {
        if (!c.x(0)) {
            std::fflush(nullptr);
            return c.ret(0);
        }
        if (auto f = stream(c, 0); f && f->fp) std::fflush(f->fp);
        c.ret(0);
    });
    reg.add("feof", [](CallContext& c) {
        auto f = stream(c, 0);
        c.ret(f && f->eof ? 1 : 0);
    });
    reg.add("ferror", [](CallContext& c) {
        auto f = stream(c, 0);
        c.ret(f && f->error ? 1 : 0);
    });
    reg.add("clearerr", [](CallContext& c) {
        if (auto f = stream(c, 0)) f->eof = f->error = false;
    });
    reg.add("setvbuf", [](CallContext& c) { c.ret(0); });
    reg.add("setbuf", [](CallContext& c) {});
    reg.add("fwide", [](CallContext& c) { c.ret(c.x(1)); });

    // --- formatted output ----------------------------------------------------------------
    reg.add("printf", [](CallContext& c) {
        RegisterArgs args(c, 1);
        std::string s = guest_format(c.mem(), c.str(0), args);
        std::fwrite(s.data(), 1, s.size(), stdout);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("fprintf", [](CallContext& c) {
        RegisterArgs args(c, 2);
        ret_int(c, print_to(c, 0, guest_format(c.mem(), c.str(1), args)));
    });
    reg.add("vfprintf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(2));
        ret_int(c, print_to(c, 0, guest_format(c.mem(), c.str(1), args)));
    });
    reg.add("sprintf", [](CallContext& c) {
        RegisterArgs args(c, 2);
        std::string s = guest_format(c.mem(), c.str(1), args);
        std::memcpy(c.str(0), s.c_str(), s.size() + 1);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("vsprintf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(2));
        std::string s = guest_format(c.mem(), c.str(1), args);
        std::memcpy(c.str(0), s.c_str(), s.size() + 1);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("snprintf", [](CallContext& c) {
        RegisterArgs args(c, 3);
        std::string s = guest_format(c.mem(), c.str(2), args);
        store_truncated(c, c.x(0), c.x(1), s);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("vsnprintf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(3));
        std::string s = guest_format(c.mem(), c.str(2), args);
        store_truncated(c, c.x(0), c.x(1), s);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    // __vsnprintf_chk(buf, size, flags, buf_size, fmt, va)
    reg.add("__vsnprintf_chk", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(5));
        std::string s = guest_format(c.mem(), c.str(4), args);
        store_truncated(c, c.x(0), c.x(1), s);
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("vasprintf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(2));
        std::string s = guest_format(c.mem(), c.str(1), args);
        c.mem().write<GuestAddr>(c.x(0), c.rt().heap().strdup(s));
        ret_int(c, static_cast<std::int64_t>(s.size()));
    });
    reg.add("swprintf", [](CallContext& c) {
        RegisterArgs args(c, 3);
        wide_print(c, args);
    });
    reg.add("vswprintf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(3));
        wide_print(c, args);
    });

    // --- formatted input -----------------------------------------------------------------
    reg.add("sscanf", [](CallContext& c) {
        RegisterArgs args(c, 2);
        ret_int(c, guest_scan(c.mem(), c.str(0), c.str(1), args));
    });
    reg.add("vsscanf", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(2));
        ret_int(c, guest_scan(c.mem(), c.str(0), c.str(1), args));
    });
    reg.add("swscanf", [](CallContext& c) {
        RegisterArgs args(c, 2);
        std::string input = u32_to_utf8(c.mem().read_wstr(c.x(0)));
        std::string fmt = u32_to_utf8(c.mem().read_wstr(c.x(1)));
        ret_int(c, guest_scan(c.mem(), input.c_str(), fmt.c_str(), args));
    });
    reg.add("fscanf", [](CallContext& c) {
        auto f = stream(c, 0);
        if (!f) return ret_int(c, -1);
        // Scan the rest of the file, then rewind past whatever wasn't consumed.
        std::int64_t start = f->tell();
        std::string rest;
        char buf[4096];
        for (std::int64_t n; (n = f->read(buf, sizeof buf)) > 0;) rest.append(buf, static_cast<std::size_t>(n));
        RegisterArgs args(c, 2);
        std::size_t used = 0;
        int r = guest_scan(c.mem(), rest.c_str(), c.str(1), args, &used);
        f->seek(start + static_cast<std::int64_t>(used), SEEK_SET);
        if (used < rest.size()) f->eof = false;
        ret_int(c, r);
    });

}

}  // namespace rfu

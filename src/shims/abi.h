#pragma once

// Android (bionic, arm64) ABI details the shims need: constants, struct layouts, and the
// argument readers for variadic functions. Values are bionic's, not the host's.

#include "core/guest_memory.h"

#include <array>
#include <cstdint>
#include <string>

namespace rfu {

class CallContext;

namespace android {

// errno
constexpr int kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kEAGAIN = 11, kENOMEM = 12, kEACCES = 13,
              kEBUSY = 16, kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kENOSPC = 28,
              kESPIPE = 29, kERANGE = 34, kEDEADLK = 35, kENOSYS = 38, kENOTEMPTY = 39, kENOTSOCK = 88,
              kEAFNOSUPPORT = 97, kETIMEDOUT = 110;

// open()
constexpr int kO_ACCMODE = 3, kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_CREAT = 0100, kO_EXCL = 0200,
              kO_TRUNC = 01000, kO_APPEND = 02000, kO_DIRECTORY = 040000;

// st_mode
constexpr std::uint32_t kS_IFMT = 0170000, kS_IFDIR = 0040000, kS_IFREG = 0100000;

// struct stat (arm64): 128 bytes
namespace stat_off {
constexpr int dev = 0, ino = 8, mode = 16, nlink = 20, uid = 24, gid = 28, rdev = 32, size = 48, blksize = 56,
              blocks = 64, atime = 72, mtime = 88, ctime = 104;
constexpr int sizeof_stat = 128;
}  // namespace stat_off

// struct tm: 9 ints, then long tm_gmtoff and const char* tm_zone
constexpr int kSizeofTm = 56;

constexpr std::uint64_t kSizeofFile = 152;  // sizeof(FILE)

}  // namespace android

// Reads the variable arguments of a call, in AAPCS64 order.
class VarArgs {
public:
    virtual ~VarArgs() = default;
    virtual std::uint64_t next_int() = 0;
    virtual double next_double() = 0;
    virtual std::array<std::uint64_t, 2> next_quad() = 0;  // long double (IEEE binary128)
};

// The `...` of the shim being called: remaining x registers, then v registers, then the
// stack. `first_int`/`first_fp` count the named arguments already used.
class RegisterArgs final : public VarArgs {
public:
    RegisterArgs(const CallContext& c, int first_int, int first_fp = 0);
    std::uint64_t next_int() override;
    double next_double() override;
    std::array<std::uint64_t, 2> next_quad() override;

private:
    std::uint64_t next_stack(std::uint64_t align);
    const CallContext& c_;
    int ngrn_, nsrn_;
    GuestAddr stack_;
};

// A guest va_list (AArch64: { __stack, __gr_top, __vr_top, __gr_offs, __vr_offs }). Note
// that va_list parameters are passed by reference, so the shim's argument is its address.
class VaListArgs final : public VarArgs {
public:
    VaListArgs(GuestMemory& mem, GuestAddr va_list);
    std::uint64_t next_int() override;
    double next_double() override;
    std::array<std::uint64_t, 2> next_quad() override;

private:
    GuestAddr next_gr();
    GuestAddr next_vr();
    GuestAddr next_stack(std::uint64_t size, std::uint64_t align);
    GuestMemory& mem_;
    GuestAddr stack_, gr_top_, vr_top_;
    std::int32_t gr_offs_, vr_offs_;
};

// long double on arm64 is IEEE binary128; the host has at best 80-bit or plain double.
double quad_to_double(std::array<std::uint64_t, 2> q);
std::array<std::uint64_t, 2> double_to_quad(double d);

// printf/scanf with bionic semantics, over guest memory.
std::string guest_format(GuestMemory& mem, const char* fmt, VarArgs& args);
// guest_scan returns the number of assignments (or -1) and, optionally, the input consumed.
int guest_scan(GuestMemory& mem, const char* input, const char* fmt, VarArgs& args, std::size_t* consumed = nullptr);

// Android's wchar_t is 32 bits wide.
std::u32string utf8_to_u32(std::string_view s);
std::string u32_to_utf8(std::u32string_view s);
std::size_t encode_utf8(char32_t c, char* out);  // writes up to 4 bytes
// Decodes one character; returns its length, 0 for NUL, -1 for invalid, -2 for incomplete.
int decode_utf8(const char* s, std::size_t n, char32_t* out);

}  // namespace rfu

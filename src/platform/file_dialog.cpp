#include "platform/file_dialog.h"

#include "core/log.h"
#include "core/vfs.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <cstdio>
#endif

#include <vector>

namespace rfu::platform {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// "Zip files (*.zip)\0*.zip\0All files\0*.*\0\0" (the filter is a list of NUL-separated pairs).
std::wstring filter_for(const std::string& extension) {
    std::wstring f;
    if (!extension.empty()) {
        std::wstring ext = widen(extension);
        f += ext + L" files (*." + ext + L")";
        f.push_back(L'\0');
        f += L"*." + ext;
        f.push_back(L'\0');
    }
    f += L"All files";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

std::optional<std::filesystem::path> run(bool save, const std::string& title, const std::string& extension, const std::string& name) {
    std::vector<wchar_t> file(32768, L'\0');
    std::wstring initial = widen(name);
    std::copy(initial.begin(), initial.begin() + std::min(initial.size(), file.size() - 1), file.begin());
    std::wstring wtitle = widen(title), filter = filter_for(extension), def_ext = widen(extension);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();  // the game's window: the dialog sits over it
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = def_ext.empty() ? nullptr : def_ext.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return std::nullopt;
    return std::filesystem::path(file.data());
}

}  // namespace

std::optional<std::filesystem::path> open_file_dialog(const std::string& title, const std::string& extension) {
    return run(false, title, extension, {});
}

std::optional<std::filesystem::path> save_file_dialog(const std::string& title, const std::string& suggested_name) {
    auto dot = suggested_name.rfind('.');
    return run(true, title, dot == std::string::npos ? std::string() : suggested_name.substr(dot + 1), suggested_name);
}

#else

namespace {

// Linux and the like: zenity, if it's there.
std::optional<std::filesystem::path> zenity(const std::string& args) {
    std::string cmd = "zenity --file-selection " + args + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return std::nullopt;
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    int status = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (status != 0 || out.empty()) return std::nullopt;
    return path_from_utf8(out);
}

std::string quoted(const std::string& s) {
    std::string q = "'";
    for (char ch : s) q += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return q + "'";
}

}  // namespace

std::optional<std::filesystem::path> open_file_dialog(const std::string& title, const std::string& extension) {
    std::string args = "--title=" + quoted(title);
    if (!extension.empty()) args += " --file-filter=" + quoted("*." + extension) + " --file-filter='*'";
    return zenity(args);
}

std::optional<std::filesystem::path> save_file_dialog(const std::string& title, const std::string& suggested_name) {
    return zenity("--save --confirm-overwrite --title=" + quoted(title) + " --filename=" + quoted(suggested_name));
}

#endif

}  // namespace rfu::platform

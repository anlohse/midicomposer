#include "file_dialogs.hpp"

#include <vector>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

namespace midi_composer::shell {

namespace {

std::string wide_to_utf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace

std::optional<std::string> open_file_dialog(const wchar_t* filter) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wide_to_utf8(file);
}

std::optional<std::string> save_file_dialog(const wchar_t* filter, const wchar_t* default_extension) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = default_extension;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return wide_to_utf8(file);
}

std::optional<std::string> open_folder_dialog() {
    // IFileDialog with FOS_PICKFOLDERS rather than SHBrowseForFolder: the old
    // one shows a tree with no path box and no recent places, which is a poor
    // way to reach a folder the user just downloaded something into.
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }

    std::optional<std::string> chosen;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)) &&
        SUCCEEDED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST)) &&
        SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR wide = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide))) {
                chosen = wide_to_utf8(wide);
                CoTaskMemFree(wide);
            }
            item->Release();
        }
    }
    dialog->Release();
    return chosen;
}

namespace {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

} // namespace

std::wstring make_file_filter(const std::string& pattern) {
    std::vector<std::string> patterns;
    size_t start = 0;
    while (true) {
        const size_t sep = pattern.find(';', start);
        const auto piece = pattern.substr(
            start, sep == std::string::npos ? std::string::npos : sep - start);
        const auto first = piece.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const auto last = piece.find_last_not_of(" \t");
            patterns.push_back(piece.substr(first, last - first + 1));
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }

    // Each entry is a label and a pattern, both NUL terminated, and the list
    // itself ends with an empty entry -- which is why this cannot be built with
    // ordinary string concatenation.
    std::wstring filter;
    if (!patterns.empty()) {
        std::string joined;
        for (const auto& one : patterns) {
            if (!joined.empty()) joined += ';';
            joined += one;
        }
        const auto wide = utf8_to_wide(joined);
        filter += L"Supported files (" + wide + L")";
        filter.push_back(L'\0');
        filter += wide;
        filter.push_back(L'\0');
    }
    filter += L"All Files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

void reveal_folder(const std::string& utf8_path) {
    if (utf8_path.empty()) return;
    const auto wide = utf8_to_wide(utf8_path);
    // Nothing is reported back: the request is "show me this", and a user who
    // watches no window appear has already learned everything an error message
    // would have told them.
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string webview2_runtime_version() {
    // The Evergreen runtime registers itself under EdgeUpdate. Machine-wide
    // installs land in the 32-bit view whatever this process is, and a per-user
    // install lands in HKCU -- so both are asked before giving up.
    static constexpr const wchar_t* kClient =
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients"
        L"\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

    for (const auto& [root, flags] :
         {std::pair{HKEY_LOCAL_MACHINE, KEY_READ | KEY_WOW64_32KEY},
          std::pair{HKEY_CURRENT_USER, KEY_READ}}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kClient, 0, static_cast<REGSAM>(flags), &key) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t version[128] = L"";
        DWORD size = sizeof(version);
        DWORD type = 0;
        const auto status = RegQueryValueExW(key, L"pv", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(version), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && type == REG_SZ && version[0] != L'\0') {
            return wide_to_utf8(version);
        }
    }
    return "not found -- the Evergreen runtime does not appear to be installed";
}

void show_error_dialog(const std::string& title, const std::string& message) {
    MessageBoxW(nullptr, utf8_to_wide(message).c_str(), utf8_to_wide(title).c_str(),
                MB_OK | MB_ICONERROR);
}

} // namespace midi_composer::shell

#else

#include <cstdio>

namespace midi_composer::shell {

std::optional<std::string> open_file_dialog(const wchar_t*) { return std::nullopt; }
std::optional<std::string> save_file_dialog(const wchar_t*, const wchar_t*) { return std::nullopt; }
std::optional<std::string> open_folder_dialog() { return std::nullopt; }
void reveal_folder(const std::string&) {}
std::string webview2_runtime_version() { return "not applicable on this platform"; }

std::wstring make_file_filter(const std::string&) {
    std::wstring filter = L"All Files (*.*)";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    return filter;
}

std::string webview2_runtime_version() {
    // The Evergreen runtime registers itself under EdgeUpdate. Machine-wide
    // installs land in the 32-bit view whatever this process is, and a per-user
    // install lands in HKCU -- so both are asked before giving up.
    static constexpr const wchar_t* kClient =
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients"
        L"\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

    for (const auto& [root, flags] :
         {std::pair{HKEY_LOCAL_MACHINE, KEY_READ | KEY_WOW64_32KEY},
          std::pair{HKEY_CURRENT_USER, KEY_READ}}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kClient, 0, static_cast<REGSAM>(flags), &key) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t version[128] = L"";
        DWORD size = sizeof(version);
        DWORD type = 0;
        const auto status = RegQueryValueExW(key, L"pv", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(version), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && type == REG_SZ && version[0] != L'\0') {
            return wide_to_utf8(version);
        }
    }
    return "not found -- the Evergreen runtime does not appear to be installed";
}

void show_error_dialog(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
}

} // namespace midi_composer::shell

#endif

#include "file_dialogs.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

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

void show_error_dialog(const std::string& title, const std::string& message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
}

} // namespace midi_composer::shell

#endif

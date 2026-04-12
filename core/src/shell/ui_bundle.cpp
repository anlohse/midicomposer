#include "ui_bundle.hpp"
#include "base/logger.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace midi_composer::shell {

namespace {

base::Error io_error(std::string message) {
    return base::Error{base::ErrorCode::IoFailure, std::move(message)};
}

std::string to_lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Zip entries may carry either separator and a "./" prefix depending on the
// packer; the scheme handler looks up plain forward-slash paths.
std::string normalize(std::string_view raw) {
    std::string path{raw};
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./")) path.erase(0, 2);
    while (path.starts_with('/')) path.erase(0, 1);
    return path;
}

} // namespace

std::string UiBundle::mime_for(const std::string& path) {
    const auto dot = path.rfind('.');
    const auto ext = dot == std::string::npos ? std::string{} : to_lower(path.substr(dot + 1));

    // Text types carry a charset: the UI has non-ASCII in its strings, and
    // without it the webview falls back to a locale-dependent guess.
    struct Mapping {
        std::string_view extension;
        std::string_view mime;
    };
    static constexpr std::array kTypes{
        Mapping{"html", "text/html; charset=utf-8"},
        Mapping{"htm", "text/html; charset=utf-8"},
        Mapping{"js", "text/javascript; charset=utf-8"},
        Mapping{"mjs", "text/javascript; charset=utf-8"},
        Mapping{"css", "text/css; charset=utf-8"},
        Mapping{"json", "application/json; charset=utf-8"},
        Mapping{"map", "application/json; charset=utf-8"},
        Mapping{"svg", "image/svg+xml; charset=utf-8"},
        Mapping{"txt", "text/plain; charset=utf-8"},
        Mapping{"png", "image/png"},
        Mapping{"jpg", "image/jpeg"},
        Mapping{"jpeg", "image/jpeg"},
        Mapping{"gif", "image/gif"},
        Mapping{"webp", "image/webp"},
        Mapping{"ico", "image/x-icon"},
        Mapping{"woff", "font/woff"},
        Mapping{"woff2", "font/woff2"},
        Mapping{"ttf", "font/ttf"},
        Mapping{"otf", "font/otf"},
        Mapping{"wasm", "application/wasm"},
    };

    for (const auto& entry : kTypes) {
        if (ext == entry.extension) return std::string{entry.mime};
    }
    return "application/octet-stream";
}

base::Result<std::unique_ptr<UiBundle>> UiBundle::load(const std::filesystem::path& archive) {
    std::error_code ec;
    if (!std::filesystem::exists(archive, ec)) {
        return std::unexpected(io_error("UI bundle not found: " + archive.string()));
    }

    mz_zip_archive zip{};

#ifdef _WIN32
    // Opened by hand: miniz uses fopen, which on Windows takes the ANSI code
    // page and so cannot reach a path with characters outside it — an install
    // under a user folder with an accent, say. mz_zip_reader_end does not close
    // a handle it did not open, so this owns it.
    std::FILE* raw = nullptr;
    _wfopen_s(&raw, archive.wstring().c_str(), L"rb");
    std::unique_ptr<std::FILE, decltype(&std::fclose)> handle{raw, &std::fclose};
    if (!handle) {
        return std::unexpected(io_error("Cannot open UI bundle: " + archive.string()));
    }
    const bool opened = mz_zip_reader_init_cfile(&zip, handle.get(), 0, 0);
#else
    const bool opened = mz_zip_reader_init_file(&zip, archive.string().c_str(), 0);
#endif
    if (!opened) {
        return std::unexpected(io_error("UI bundle is not a readable zip: " + archive.string()));
    }

    auto bundle = std::make_unique<UiBundle>();
    const auto count = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(io_error("Corrupt entry in UI bundle"));
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        const auto name = normalize(stat.m_filename);
        if (name.empty()) continue;

        Entry entry;
        entry.mime = mime_for(name);
        entry.data.resize(static_cast<std::size_t>(stat.m_uncomp_size));

        if (!entry.data.empty() &&
            !mz_zip_reader_extract_to_mem(&zip, i, entry.data.data(), entry.data.size(), 0)) {
            mz_zip_reader_end(&zip);
            return std::unexpected(io_error("Failed to decompress '" + name + "' from the UI bundle"));
        }

        bundle->m_entries.insert_or_assign(name, std::move(entry));
    }

    mz_zip_reader_end(&zip);

    if (!bundle->contains("index.html")) {
        return std::unexpected(io_error("UI bundle has no index.html: " + archive.string()));
    }

    MC_LOG_INFO("Loaded UI bundle: {} files, {} bytes from {}",
                bundle->size(), bundle->bytes(), archive.string());
    return bundle;
}

std::map<std::string, saucer::embedded_file> UiBundle::embedded() const {
    std::map<std::string, saucer::embedded_file> files;
    for (const auto& [path, entry] : m_entries) {
        files.emplace(path, saucer::embedded_file{entry.mime, std::span{entry.data}});
    }
    return files;
}

bool UiBundle::contains(const std::string& path) const {
    return m_entries.contains(path);
}

std::size_t UiBundle::bytes() const {
    std::size_t total = 0;
    for (const auto& [_, entry] : m_entries) total += entry.data.size();
    return total;
}

std::filesystem::path default_ui_bundle_path() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto written = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (written == 0) break;
        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path{buffer}.parent_path() / "ui.pak";
        }
        buffer.resize(buffer.size() * 2);   // path longer than MAX_PATH
    }
#endif
    // Last resort. Launching from another directory will not find the bundle,
    // which the caller reports rather than starting with a blank window.
    std::error_code ec;
    return std::filesystem::current_path(ec) / "ui.pak";
}

} // namespace midi_composer::shell

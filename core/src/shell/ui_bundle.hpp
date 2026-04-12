#pragma once

#include "base/error.hpp"

#include <saucer/webview.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace midi_composer::shell {

// The production UI, unpacked from the zip archive shipped next to the
// executable and handed to the webview through saucer's embedded-file scheme.
//
// Lifetime matters: saucer::embedded_file holds a std::span, not a copy, so the
// bytes have to outlive the webview. The bundle is therefore handed out as a
// unique_ptr — its address, and so every span into it, stays put — and must be
// kept alive for as long as the window is open.
class UiBundle {
  public:
    // Reads and decompresses every entry. Fails rather than starting with a
    // half-loaded UI: a missing bundle is a packaging error, and a blank window
    // with no explanation is the worst way to report one.
    [[nodiscard]] static base::Result<std::unique_ptr<UiBundle>> load(
        const std::filesystem::path& archive);

    // Keyed the way saucer's scheme handler looks entries up: the URL path with
    // no leading slash, e.g. "index.html", "public.a1b2c3.js".
    [[nodiscard]] std::map<std::string, saucer::embedded_file> embedded() const;

    [[nodiscard]] bool contains(const std::string& path) const;
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }
    [[nodiscard]] std::size_t bytes() const;

    // Content type served for a path, from its extension. Exposed for testing.
    [[nodiscard]] static std::string mime_for(const std::string& path);

  private:
    struct Entry {
        std::string mime;
        std::vector<std::uint8_t> data;
    };

    std::map<std::string, Entry> m_entries;
};

// Where the packed UI is expected: alongside the executable, not the working
// directory, which is wherever the user happened to launch from.
[[nodiscard]] std::filesystem::path default_ui_bundle_path();

} // namespace midi_composer::shell

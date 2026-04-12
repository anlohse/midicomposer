#include <doctest/doctest.h>

#include "shell/ui_bundle.hpp"

#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace midi_composer;

namespace {

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

struct ScopedFile {
    std::filesystem::path path;
    ~ScopedFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};

struct ZipEntry {
    std::string name;
    std::string content;
};

// Writes a real zip, so the tests exercise the same path production does rather
// than a stand-in for it.
bool write_zip(const std::filesystem::path& path, const std::vector<ZipEntry>& entries) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.string().c_str(), 0)) return false;
    for (const auto& entry : entries) {
        if (!mz_zip_writer_add_mem(&zip, entry.name.c_str(), entry.content.data(),
                                   entry.content.size(), MZ_BEST_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    const bool ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

std::string text_of(const saucer::embedded_file& file) {
    return std::string{reinterpret_cast<const char*>(file.content.data()), file.content.size()};
}

const std::string kIndex = "<!DOCTYPE html><body><mc-app-root></mc-app-root></body>";

} // namespace

TEST_CASE("a packed bundle round-trips through the archive") {
    ScopedFile file{temp_file("mc_ui_bundle_test.pak")};
    // Compressible on purpose: the point of the archive is that it is deflated,
    // and a stored-only entry would not exercise decompression.
    const std::string script(20000, 'a');
    REQUIRE(write_zip(file.path, {{"index.html", kIndex}, {"public.abc123.js", script}}));

    auto bundle = shell::UiBundle::load(file.path);
    REQUIRE(bundle.has_value());
    CHECK((*bundle)->size() == 2);
    CHECK((*bundle)->bytes() == kIndex.size() + script.size());

    // The packed file must be meaningfully smaller than what it carries, or the
    // archive is doing nothing.
    CHECK(std::filesystem::file_size(file.path) < script.size() / 2);

    const auto files = (*bundle)->embedded();
    REQUIRE(files.contains("index.html"));
    REQUIRE(files.contains("public.abc123.js"));
    CHECK(text_of(files.at("index.html")) == kIndex);
    CHECK(text_of(files.at("public.abc123.js")) == script);
}

TEST_CASE("entries are keyed by the URL path the webview asks for") {
    ScopedFile file{temp_file("mc_ui_bundle_paths.pak")};
    // Packers differ on separators and on a leading "./"; the scheme handler
    // looks up a plain relative path either way.
    REQUIRE(write_zip(file.path, {{"index.html", kIndex},
                                  {"./assets/logo.svg", "<svg/>"},
                                  {"fonts\\face.woff2", "font"}}));

    auto bundle = shell::UiBundle::load(file.path);
    REQUIRE(bundle.has_value());
    CHECK((*bundle)->contains("assets/logo.svg"));
    CHECK((*bundle)->contains("fonts/face.woff2"));
    CHECK_FALSE((*bundle)->contains("./assets/logo.svg"));
}

TEST_CASE("content types are served from the extension") {
    CHECK(shell::UiBundle::mime_for("index.html") == "text/html; charset=utf-8");
    CHECK(shell::UiBundle::mime_for("public.a1b2.js") == "text/javascript; charset=utf-8");
    CHECK(shell::UiBundle::mime_for("style.css") == "text/css; charset=utf-8");
    CHECK(shell::UiBundle::mime_for("logo.svg") == "image/svg+xml; charset=utf-8");
    CHECK(shell::UiBundle::mime_for("face.woff2") == "font/woff2");
    CHECK(shell::UiBundle::mime_for("art.PNG") == "image/png");     // case-insensitive
    CHECK(shell::UiBundle::mime_for("LICENSE") == "application/octet-stream");
    CHECK(shell::UiBundle::mime_for("archive.tar.gz") == "application/octet-stream");
}

TEST_CASE("an ES module is served as JavaScript") {
    // The entry point is <script type="module">; a webview refuses to run a
    // module served as anything but a JavaScript type.
    ScopedFile file{temp_file("mc_ui_bundle_mime.pak")};
    REQUIRE(write_zip(file.path, {{"index.html", kIndex}, {"public.js", "export {};"}}));

    auto bundle = shell::UiBundle::load(file.path);
    REQUIRE(bundle.has_value());
    CHECK((*bundle)->embedded().at("public.js").mime.starts_with("text/javascript"));
}

TEST_CASE("a bundle with no entry point is refused") {
    ScopedFile file{temp_file("mc_ui_bundle_noindex.pak")};
    REQUIRE(write_zip(file.path, {{"public.js", "export {};"}}));

    // Serving a bundle without index.html would open a blank window, which
    // reads as a crash rather than as a packaging mistake.
    auto bundle = shell::UiBundle::load(file.path);
    REQUIRE_FALSE(bundle.has_value());
    CHECK(bundle.error().code == base::ErrorCode::IoFailure);
    CHECK(bundle.error().message.find("index.html") != std::string::npos);
}

TEST_CASE("a missing bundle fails cleanly") {
    auto bundle = shell::UiBundle::load(temp_file("mc_ui_bundle_absent.pak"));
    REQUIRE_FALSE(bundle.has_value());
    CHECK(bundle.error().message.find("not found") != std::string::npos);
}

TEST_CASE("a file that is not a zip fails cleanly") {
    ScopedFile file{temp_file("mc_ui_bundle_garbage.pak")};
    {
        std::ofstream out{file.path, std::ios::binary};
        out << "this is not an archive";
    }
    CHECK_FALSE(shell::UiBundle::load(file.path).has_value());
}

TEST_CASE("an empty file fails cleanly") {
    ScopedFile file{temp_file("mc_ui_bundle_empty.pak")};
    { std::ofstream out{file.path, std::ios::binary}; }
    CHECK_FALSE(shell::UiBundle::load(file.path).has_value());
}

TEST_CASE("the default bundle path sits next to the executable") {
    const auto path = shell::default_ui_bundle_path();
    CHECK(path.filename() == "ui.pak");
    CHECK(path.is_absolute());
    // Beside the test binary, which is where a shipped app would find its own.
    CHECK(std::filesystem::exists(path.parent_path()));
}

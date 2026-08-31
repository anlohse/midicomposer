#include <doctest/doctest.h>

#include "app/preferences.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace midi_composer;

namespace {

// A directory of its own per test, removed afterwards, so a test that writes
// cannot decide what a later one reads.
class TempDir {
public:
    TempDir() {
        m_path = std::filesystem::temp_directory_path() /
                 ("mc_prefs_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    [[nodiscard]] std::filesystem::path file() const { return m_path / "preferences.json"; }

private:
    std::filesystem::path m_path;
};

std::string read_all(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_all(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

} // namespace

TEST_CASE("Preferences survive a round trip") {
    TempDir dir;

    app::Preferences saved;
    saved.load(dir.file());   // nothing there yet; this only sets the location
    saved.set_selected_output("system-midi");
    saved.set_parameter("system-midi", "port", std::string{"Microsoft GS Wavetable Synth"});
    saved.set_parameter("internal-synth", "voices", 8);
    saved.set_parameter("internal-synth", "hermite", true);
    saved.set_clap_search_paths({"D:\\Plugins\\CLAP", "C:/Other/CLAP"});
    REQUIRE(saved.save().has_value());

    app::Preferences loaded;
    loaded.load(dir.file());

    CHECK(loaded.selected_output() == "system-midi");
    CHECK(loaded.clap_search_paths() ==
          std::vector<std::string>{"D:\\Plugins\\CLAP", "C:/Other/CLAP"});

    const auto system = loaded.parameters_for("system-midi");
    REQUIRE(system.count("port") == 1);
    CHECK(std::get<std::string>(system.at("port")) == "Microsoft GS Wavetable Synth");

    const auto synth = loaded.parameters_for("internal-synth");
    // The variant's alternative has to survive too: an int that comes back as a
    // bool would be restored onto the wrong parameter kind.
    REQUIRE(synth.count("voices") == 1);
    CHECK(std::get<int>(synth.at("voices")) == 8);
    REQUIRE(synth.count("hermite") == 1);
    CHECK(std::get<bool>(synth.at("hermite")) == true);
}

TEST_CASE("A path with backslashes is not mangled by the file") {
    // The bridge was once accused of eating these (§9b); the file must not be
    // the place it actually happens.
    TempDir dir;
    const std::string windows_path = "D:\\apps\\workspaces\\JC-303\\CLAP";

    app::Preferences saved;
    saved.load(dir.file());
    saved.set_clap_search_paths({windows_path});
    REQUIRE(saved.save().has_value());

    app::Preferences loaded;
    loaded.load(dir.file());
    REQUIRE(loaded.clap_search_paths().size() == 1);
    CHECK(loaded.clap_search_paths()[0] == windows_path);
}

TEST_CASE("Parameters are kept apart by output") {
    app::Preferences prefs;
    prefs.set_parameter("system-midi", "port", std::string{"A"});
    prefs.set_parameter("other-output", "port", std::string{"B"});

    CHECK(std::get<std::string>(prefs.parameters_for("system-midi").at("port")) == "A");
    CHECK(std::get<std::string>(prefs.parameters_for("other-output").at("port")) == "B");
    CHECK(prefs.parameters_for("never-seen").empty());
}

TEST_CASE("An unset parameter is stored as nothing, not as a value") {
    app::Preferences prefs;
    prefs.set_parameter("system-midi", "port", std::string{"A"});
    prefs.set_parameter("system-midi", "port", playback::ParameterValue{});

    CHECK(prefs.parameters_for("system-midi").count("port") == 0);
}

TEST_CASE("A missing file leaves the defaults alone") {
    TempDir dir;
    app::Preferences prefs;
    prefs.load(dir.file() / "does-not-exist.json");

    CHECK(prefs.selected_output().empty());
    CHECK(prefs.clap_search_paths().empty());
}

TEST_CASE("A corrupt file leaves the defaults alone rather than throwing") {
    TempDir dir;
    write_all(dir.file(), "{ this is not json");

    app::Preferences prefs;
    prefs.load(dir.file());   // must not throw

    CHECK(prefs.selected_output().empty());
    CHECK(prefs.clap_search_paths().empty());
}

TEST_CASE("A file from a newer version is left unread") {
    TempDir dir;
    write_all(dir.file(),
              R"({"schemaVersion": 99, "selectedOutput": "something-from-the-future"})");

    app::Preferences prefs;
    prefs.load(dir.file());

    // Reading it would mean guessing at fields this build does not know, and
    // the next save would write the guess back over the real thing.
    CHECK(prefs.selected_output().empty());
}

TEST_CASE("Entries of the wrong shape are skipped, not fatal") {
    TempDir dir;
    write_all(dir.file(), R"({
        "schemaVersion": 1,
        "selectedOutput": "system-midi",
        "outputParameters": { "system-midi": { "port": "Good", "broken": [1, 2] } },
        "clapSearchPaths": ["D:/Keep", 42, null]
    })");

    app::Preferences prefs;
    prefs.load(dir.file());

    CHECK(prefs.selected_output() == "system-midi");
    const auto params = prefs.parameters_for("system-midi");
    CHECK(std::get<std::string>(params.at("port")) == "Good");
    CHECK(params.count("broken") == 0);
    CHECK(prefs.clap_search_paths() == std::vector<std::string>{"D:/Keep"});
}

TEST_CASE("Saving replaces the file rather than appending to it") {
    TempDir dir;

    app::Preferences first;
    first.load(dir.file());
    first.set_selected_output("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(first.save().has_value());
    const auto long_version = read_all(dir.file());

    app::Preferences second;
    second.load(dir.file());
    second.set_selected_output("b");
    REQUIRE(second.save().has_value());
    const auto short_version = read_all(dir.file());

    CHECK(short_version.size() < long_version.size());
    CHECK(short_version.find("aaaaaaaa") == std::string::npos);

    // And nothing is left beside it: an interrupted write is the only thing
    // that should ever leave a .tmp behind.
    CHECK_FALSE(std::filesystem::exists(dir.file().string() + ".tmp"));
}

TEST_CASE("Saving with nowhere to save is an error, not a crash") {
    app::Preferences prefs;   // never loaded, so it has no path
    auto result = prefs.save();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == base::ErrorCode::InvalidState);
}

TEST_CASE("The default location is under the user's profile") {
    const auto path = app::Preferences::default_path();
    // Empty is allowed -- a platform that will not say where means the
    // application runs on defaults -- but a non-empty one has to be a file.
    if (!path.empty()) {
        CHECK(path.filename() == "preferences.json");
        CHECK(path.has_parent_path());
    }
}

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

TEST_CASE("The metronome's output is remembered like any other choice") {
    TempDir dir;

    app::Preferences saved;
    saved.load(dir.file());
    saved.set_metronome_output("co.midilab.JC303");
    REQUIRE(saved.save().has_value());

    app::Preferences loaded;
    loaded.load(dir.file());
    CHECK(loaded.metronome_output() == "co.midilab.JC303");
}

TEST_CASE("An unchosen metronome output is empty, not a guessed default") {
    // The preferences do not know what outputs exist. Writing a default here
    // would record a choice the user never made, and nothing could later tell
    // it apart from one they did.
    TempDir dir;
    app::Preferences prefs;
    prefs.load(dir.file());
    CHECK(prefs.metronome_output().empty());

    REQUIRE(prefs.save().has_value());
    app::Preferences reloaded;
    reloaded.load(dir.file());
    CHECK(reloaded.metronome_output().empty());
}

TEST_CASE("Clearing the metronome output is how the reset is recorded") {
    TempDir dir;
    app::Preferences prefs;
    prefs.load(dir.file());
    prefs.set_metronome_output("a-plugin-that-was-uninstalled");
    REQUIRE(prefs.save().has_value());

    // What the facade does when the named output is gone: back to empty, and
    // written, so the next session starts from the default rather than finding
    // the same dead name again.
    prefs.set_metronome_output({});
    REQUIRE(prefs.save().has_value());

    app::Preferences reloaded;
    reloaded.load(dir.file());
    CHECK(reloaded.metronome_output().empty());
    CHECK(read_all(dir.file()).find("a-plugin-that-was-uninstalled") == std::string::npos);
}

TEST_CASE("The file says which application it belongs to") {
    TempDir dir;
    app::Preferences prefs;
    prefs.load(dir.file());
    REQUIRE(prefs.save().has_value());

    // Not just valid JSON: recognisable. preferences.json is a name many
    // programs use, so the file has to identify itself.
    CHECK(read_all(dir.file()).find("\"application\": \"MIDI Composer\"") != std::string::npos);
}

TEST_CASE("Another application's file is not adopted as ours") {
    TempDir dir;
    write_all(dir.file(), R"({
        "application": "Some Other DAW",
        "schemaVersion": 1,
        "selectedOutput": "their-output",
        "clapSearchPaths": ["D:/Theirs"]
    })");

    app::Preferences prefs;
    prefs.load(dir.file());

    // Reading it would mean playing through a device chosen in another program,
    // which is worse than starting from defaults.
    CHECK(prefs.selected_output().empty());
    CHECK(prefs.clap_search_paths().empty());
}

TEST_CASE("A file without the marker is still read, and gains one when saved") {
    // The marker was added after the format was, so a file predating it may
    // well be ours; discarding it would throw away settings to enforce a field
    // that did not exist when they were written.
    TempDir dir;
    write_all(dir.file(), R"({"schemaVersion": 1, "selectedOutput": "internal-synth"})");

    app::Preferences prefs;
    prefs.load(dir.file());
    CHECK(prefs.selected_output() == "internal-synth");

    REQUIRE(prefs.save().has_value());
    CHECK(read_all(dir.file()).find("MIDI Composer") != std::string::npos);
}

TEST_CASE("The application owns a plugin folder of its own") {
    const auto folder = app::Preferences::plugin_folder();
    if (!folder.empty()) {
        // Separate from the preferences file: these are native binaries, and a
        // roaming profile would carry one machine's build onto another.
        CHECK(folder != app::Preferences::default_path().parent_path());
        CHECK(folder.has_parent_path());
    }
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

// ── Remembered layout ────────────────────────────────────────────────────────
//
// A window size read back from a file is a size somebody could have
// hand-edited, or one left over from a monitor that is no longer attached.
// Restoring a window nobody can see or resize leaves no obvious way out except
// finding this file, so what comes out of here is either usable or nothing.

TEST_CASE("a window size survives a round trip") {
    app::Preferences written;
    written.set_window(1008, 601, false);

    app::Preferences read;
    read.from_json(written.to_json());
    CHECK(read.window_width() == 1008);
    CHECK(read.window_height() == 601);
    CHECK_FALSE(read.window_maximized());
}

TEST_CASE("maximized is remembered apart from the size") {
    // Both, deliberately: the size is what un-maximizing should land on, so
    // one must not replace the other.
    app::Preferences written;
    written.set_window(1280, 720, true);

    app::Preferences read;
    read.from_json(written.to_json());
    CHECK(read.window_maximized());
    CHECK(read.window_width() == 1280);
}

TEST_CASE("nothing remembered yet reads as nothing, not as zero by accident") {
    app::Preferences fresh;
    CHECK(fresh.window_width() == 0);
    CHECK(fresh.window_height() == 0);
    // And a first run writes no window at all rather than a size nobody chose.
    CHECK(fresh.to_json().find("\"window\"") == std::string::npos);
}

TEST_CASE("a window too small to use is refused") {
    app::Preferences read;
    read.from_json(R"({"application":"MIDI Composer","window":{"width":1200,"height":4}})");
    CHECK(read.window_width() == 0);
    CHECK(read.window_height() == 0);
}

TEST_CASE("a window larger than any desktop is refused") {
    app::Preferences read;
    read.from_json(R"({"application":"MIDI Composer","window":{"width":99999,"height":800}})");
    CHECK(read.window_width() == 0);
}

TEST_CASE("the interface's own layout is stored and handed back unchanged") {
    // Opaque on purpose: adding a fourth panel toggle must not need a change
    // in this file.
    app::Preferences written;
    written.set_ui_layout(nlohmann::json{{"mixerCollapsed", true},
                                         {"showRuler", false},
                                         {"somethingNew", 7}});

    app::Preferences read;
    read.from_json(written.to_json());
    CHECK(read.ui_layout().at("mixerCollapsed").get<bool>());
    CHECK_FALSE(read.ui_layout().at("showRuler").get<bool>());
    CHECK(read.ui_layout().at("somethingNew").get<int>() == 7);
}

TEST_CASE("a layout that is not an object is ignored rather than adopted") {
    app::Preferences read;
    read.from_json(R"({"application":"MIDI Composer","uiLayout":"not an object"})");
    CHECK(read.ui_layout().empty());
}

TEST_CASE("maximized alone is remembered, with no size beside it") {
    // Somebody who maximizes on a first run and closes has expressed a
    // preference. Requiring a width before writing it down would drop it.
    app::Preferences written;
    written.set_window(0, 0, true);
    CHECK(written.to_json().find("\"window\"") != std::string::npos);

    app::Preferences read;
    read.from_json(written.to_json());
    CHECK(read.window_maximized());
    CHECK(read.window_width() == 0);
}

TEST_CASE("a maximized window's own size is never the size to come back to") {
    // The shell keeps the last un-maximized size while maximized, and this is
    // the shape that has to survive: a flag, and a size measured earlier.
    //
    // Storing the maximized size instead lost the size the window had before
    // it was maximized, so un-maximizing after a restore filled the screen
    // rather than returning the window to where it was left.
    app::Preferences written;
    written.set_window(1008, 601, false);   // measured un-maximized
    written.set_window(1008, 601, true);    // then maximized, size unchanged

    app::Preferences read;
    read.from_json(written.to_json());
    CHECK(read.window_maximized());
    CHECK(read.window_width() == 1008);
    CHECK(read.window_height() == 601);
}

#include <doctest/doctest.h>

#include "io/midi_file.hpp"
#include "music/composition.hpp"
#include "project/project_serializer.hpp"

#include <filesystem>
#include <string>

using namespace midi_composer;

namespace {

// A composition exercising every field the formats are supposed to preserve.
music::Composition make_composition() {
    music::Composition comp{base::CompositionId{1}};
    comp.set_title("Fixture");
    comp.set_ppqn(480);
    comp.set_master_volume(80);

    comp.tempo_map().events().clear();
    comp.tempo_map().events().push_back({base::EventId{1}, timeline::Tick{0}, 500000});

    comp.time_signature_map().events().clear();
    comp.time_signature_map().events().push_back({base::EventId{2}, timeline::Tick{0}, 6, 8});
    comp.time_signature_map().events().push_back({base::EventId{3}, timeline::Tick{2880}, 3, 4});

    comp.key_signature_map().events().clear();
    comp.key_signature_map().events().push_back({base::EventId{4}, timeline::Tick{0}, -3, true});
    comp.key_signature_map().events().push_back({base::EventId{5}, timeline::Tick{2880}, 2, false});

    music::Track lead{base::TrackId{1}, "Lead"};
    lead.set_midi_channel(3);
    lead.set_clef(music::Clef::Treble8va);
    lead.set_volume(90);
    lead.set_pan(40);
    lead.set_muted(true);
    lead.program_changes().push_back({base::EventId{10}, timeline::Tick{0}, 40});
    lead.controller_events().push_back({base::EventId{11}, timeline::Tick{240}, 7, 100});
    lead.pitch_bends().push_back({base::EventId{12}, timeline::Tick{480}, -2048});
    for (int i = 0; i < 3; ++i) {
        music::Note n;
        n.id = base::NoteId{static_cast<std::uint64_t>(i + 1)};
        n.start = timeline::Tick{i * 480};
        n.duration = timeline::TickDuration{480};
        n.pitch = static_cast<std::uint8_t>(72 + i);
        n.velocity = static_cast<std::uint8_t>(90 + i);
        lead.notes().push_back(n);
    }
    comp.tracks().push_back(std::move(lead));

    music::Track bass{base::TrackId{2}, "Bass"};
    bass.set_clef(music::Clef::Bass8vb);
    bass.set_midi_channel(1);
    music::Note low;
    low.id = base::NoteId{20};
    low.start = timeline::Tick{0};
    low.duration = timeline::TickDuration{960};
    low.pitch = 31;
    low.velocity = 80;
    bass.notes().push_back(low);
    comp.tracks().push_back(std::move(bass));

    return comp;
}

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

struct ScopedFile {
    std::filesystem::path path;
    ~ScopedFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};

} // namespace

TEST_CASE("the native format round-trips every persisted field") {
    const auto original = make_composition();
    const auto json = project::ProjectSerializer::to_json(original);
    auto restored = project::ProjectSerializer::from_json(json);
    REQUIRE(restored.has_value());

    CHECK(restored->title() == "Fixture");
    CHECK(restored->ppqn() == 480);

    REQUIRE(restored->time_signature_map().events().size() == 2);
    CHECK(restored->time_signature_map().events()[1].tick.value() == 2880);
    CHECK(restored->time_signature_map().events()[1].numerator == 3);

    // Every event map has to be carried; a forgotten one is silently dropped.
    REQUIRE(restored->key_signature_map().events().size() == 2);
    CHECK(restored->key_signature_map().events()[0].fifths == -3);
    CHECK(restored->key_signature_map().events()[0].minor == true);
    CHECK(restored->key_signature_map().events()[1].fifths == 2);

    REQUIRE(restored->tracks().size() == 2);
    const auto& lead = restored->tracks()[0];
    CHECK(lead.name() == "Lead");
    CHECK(lead.midi_channel() == 3);
    CHECK(lead.clef() == music::Clef::Treble8va);
    CHECK(lead.volume() == 90);
    CHECK(lead.pan() == 40);
    CHECK(lead.is_muted());
    CHECK(lead.notes().size() == 3);
    CHECK(lead.notes()[1].pitch == 73);
    CHECK(lead.program_changes().size() == 1);
    CHECK(lead.program_changes()[0].program == 40);
    CHECK(lead.controller_events().size() == 1);
    CHECK(lead.controller_events()[0].value == 100);
    CHECK(lead.pitch_bends().size() == 1);
    CHECK(lead.pitch_bends()[0].value == -2048);

    CHECK(restored->tracks()[1].clef() == music::Clef::Bass8vb);
}

TEST_CASE("a project written without a key signature map opens in C major") {
    // Older projects predate key signatures; they must not open keyless.
    auto json = project::ProjectSerializer::to_json(make_composition());
    json.erase("keySignatureMap");
    auto restored = project::ProjectSerializer::from_json(json);
    REQUIRE(restored.has_value());
    REQUIRE(restored->key_signature_map().events().size() == 1);
    CHECK(restored->key_signature_map().events()[0].fifths == 0);
    CHECK(restored->key_signature_map().events()[0].tick.value() == 0);
}

TEST_CASE("a project from a newer format version is refused") {
    auto json = project::ProjectSerializer::to_json(make_composition());
    json["formatVersion"] = 9999;
    CHECK_FALSE(project::ProjectSerializer::from_json(json));
}

TEST_CASE("malformed project JSON is refused rather than crashing") {
    CHECK_FALSE(project::ProjectSerializer::from_json(nlohmann::json::object()));
    CHECK_FALSE(project::ProjectSerializer::from_json(nlohmann::json{{"formatVersion", 1},
                                                                    {"tempoMap", "not an array"}}));
}

TEST_CASE("saving and loading a project file preserves it") {
    ScopedFile file{temp_file("mc_persistence_test.mcproj")};
    const auto original = make_composition();

    REQUIRE(project::ProjectSerializer::save_file(original, file.path.string()).has_value());
    auto restored = project::ProjectSerializer::load_file(file.path.string());
    REQUIRE(restored.has_value());
    CHECK(restored->tracks().size() == original.tracks().size());
    CHECK(restored->key_signature_map().events().size() == 2);
}

TEST_CASE("loading a missing project file fails cleanly") {
    CHECK_FALSE(project::ProjectSerializer::load_file(
        (std::filesystem::temp_directory_path() / "mc_does_not_exist.mcproj").string()));
}

TEST_CASE("MIDI export and import preserve notes, meter and key") {
    ScopedFile file{temp_file("mc_persistence_test.mid")};
    const auto original = make_composition();

    REQUIRE(io::MidiFile::export_file(original, file.path.string()).has_value());
    auto restored = io::MidiFile::import_file(file.path.string());
    REQUIRE(restored.has_value());

    CHECK(restored->ppqn() == 480);

    // The meter and key maps travel as FF 58 / FF 59 meta events.
    REQUIRE(restored->time_signature_map().events().size() == 2);
    CHECK(restored->time_signature_map().events()[0].numerator == 6);
    CHECK(restored->time_signature_map().events()[0].denominator == 8);
    CHECK(restored->time_signature_map().events()[1].tick.value() == 2880);

    REQUIRE(restored->key_signature_map().events().size() == 2);
    CHECK(restored->key_signature_map().events()[0].fifths == -3);
    CHECK(restored->key_signature_map().events()[0].minor == true);
    CHECK(restored->key_signature_map().events()[1].fifths == 2);

    // Two tracks carried notes; conductor-only tracks are dropped on import.
    REQUIRE(restored->tracks().size() == 2);
    CHECK(restored->tracks()[0].notes().size() == 3);
    CHECK(restored->tracks()[0].notes()[0].pitch == 72);
    CHECK(restored->tracks()[0].program_changes().size() == 1);
    CHECK(restored->tracks()[0].program_changes()[0].program == 40);
    CHECK(restored->tracks()[1].notes().size() == 1);
    CHECK(restored->tracks()[1].notes()[0].pitch == 31);
}

TEST_CASE("MIDI import derives a clef from each track's register") {
    ScopedFile file{temp_file("mc_clef_derive_test.mid")};

    music::Composition comp{base::CompositionId{1}};
    comp.set_ppqn(480);
    // MIDI carries no clef, so import guesses from where the part sits.
    const int registers[] = {27, 45, 63, 91};
    std::uint64_t id = 1;
    for (int i = 0; i < 4; ++i) {
        music::Track t{base::TrackId{static_cast<std::uint64_t>(i + 1)}, "T"};
        t.set_midi_channel(static_cast<std::uint8_t>(i));
        music::Note n;
        n.id = base::NoteId{id++};
        n.start = timeline::Tick{0};
        n.duration = timeline::TickDuration{480};
        n.pitch = static_cast<std::uint8_t>(registers[i]);
        n.velocity = 100;
        t.notes().push_back(n);
        comp.tracks().push_back(std::move(t));
    }

    REQUIRE(io::MidiFile::export_file(comp, file.path.string()).has_value());
    auto restored = io::MidiFile::import_file(file.path.string());
    REQUIRE(restored.has_value());
    REQUIRE(restored->tracks().size() == 4);
    CHECK(restored->tracks()[0].clef() == music::Clef::Bass8vb);
    CHECK(restored->tracks()[1].clef() == music::Clef::Bass);
    CHECK(restored->tracks()[2].clef() == music::Clef::Treble);
    CHECK(restored->tracks()[3].clef() == music::Clef::Treble8va);
}

TEST_CASE("importing a file that is not MIDI fails cleanly") {
    CHECK_FALSE(io::MidiFile::import_file(
        (std::filesystem::temp_directory_path() / "mc_does_not_exist.mid").string()));
}

TEST_CASE("the master fader is saved and read back") {
    const auto comp = make_composition();
    const auto json = project::ProjectSerializer::to_json(comp);
    REQUIRE(json.contains("masterVolume"));

    const auto loaded = project::ProjectSerializer::from_json(json);
    REQUIRE(loaded.has_value());
    CHECK(loaded->master_volume() == 80);
}

TEST_CASE("a project saved before the master fader existed loads at unity") {
    // Anything written by an earlier build has no masterVolume at all. Defaulting
    // to 0 would open those projects silent; unity is how they were mixed.
    auto json = project::ProjectSerializer::to_json(make_composition());
    json.erase("masterVolume");

    const auto loaded = project::ProjectSerializer::from_json(json);
    REQUIRE(loaded.has_value());
    CHECK(loaded->master_volume() == 127);
}

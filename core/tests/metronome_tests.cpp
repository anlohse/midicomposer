#include <doctest/doctest.h>

#include "device/midi_service.hpp"
#include "playback/playback_engine.hpp"
#include "project/project_document.hpp"
#include "recording_output.hpp"

#include <chrono>
#include <thread>

using namespace midi_composer;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kPpqn = 480;

// The click's pitches. General MIDI wood blocks, which is what the engine uses
// and what a test has to name to tell a click from a note.
constexpr int kDownbeat = 77;
constexpr int kOffbeat  = 76;

project::ProjectDocument empty_document() {
    music::Composition comp{base::CompositionId{1}};
    comp.set_ppqn(static_cast<int>(kPpqn));
    comp.tracks().push_back(music::Track{base::TrackId{1}, "Track 1"});
    return project::ProjectDocument{std::move(comp)};
}

int clicks(const testing::RecordingOutput& out) {
    int count = 0;
    for (const auto& e : out.events()) {
        if (e.kind == testing::RecordingOutput::Event::Kind::NoteOn && e.channel == 9 &&
            (e.a == kDownbeat || e.a == kOffbeat)) {
            ++count;
        }
    }
    return count;
}

int note_offs_on_channel_nine(const testing::RecordingOutput& out) {
    int count = 0;
    for (const auto& e : out.events()) {
        if (e.kind == testing::RecordingOutput::Event::Kind::NoteOff && e.channel == 9) ++count;
    }
    return count;
}

/** Long enough for several beats at the default 120bpm (500ms a beat). */
void play_a_while(playback::PlaybackEngine& engine, project::ProjectDocument& doc) {
    REQUIRE(engine.play(doc).has_value());
    std::this_thread::sleep_for(1200ms);
    engine.stop();
}

} // namespace

TEST_CASE("with nothing named, the click follows the ordinary routing") {
    device::MidiService midi;
    testing::RecordingOutput out;
    playback::PlaybackEngine engine{midi, out};
    engine.set_metronome_enabled(true);

    auto doc = empty_document();
    play_a_while(engine, doc);

    // Null is not "no metronome": it is "wherever everything else goes", which
    // is what a project with no tracks has to fall back to.
    CHECK(clicks(out) > 0);
}

TEST_CASE("the click goes to the output it was given, not the default one") {
    // §10.1: which output that is, is the user's preference. The engine only
    // has to honour whatever it was handed.
    device::MidiService midi;
    testing::RecordingOutput ordinary;
    testing::RecordingOutput click_target;
    playback::PlaybackEngine engine{midi, ordinary};
    engine.set_metronome_enabled(true);
    engine.set_metronome_output(&click_target);

    auto doc = empty_document();
    play_a_while(engine, doc);

    CHECK(clicks(click_target) > 0);
    // Not to the project's output, and not to whoever owns channel 9.
    CHECK(clicks(ordinary) == 0);
}

TEST_CASE("a click is turned off through the output it was turned on through") {
    device::MidiService midi;
    testing::RecordingOutput ordinary;
    testing::RecordingOutput click_target;
    playback::PlaybackEngine engine{midi, ordinary};
    engine.set_metronome_enabled(true);
    engine.set_metronome_output(&click_target);

    auto doc = empty_document();
    play_a_while(engine, doc);

    // A note-off sent to the ordinary output would leave the click's output
    // holding a note nothing ever releases.
    CHECK(note_offs_on_channel_nine(click_target) > 0);
    CHECK(note_offs_on_channel_nine(ordinary) == 0);
}

TEST_CASE("moving the click mid-playback releases it on the output it leaves") {
    device::MidiService midi;
    testing::RecordingOutput ordinary;
    testing::RecordingOutput first;
    testing::RecordingOutput second;
    playback::PlaybackEngine engine{midi, ordinary};
    engine.set_metronome_enabled(true);
    engine.set_metronome_output(&first);

    auto doc = empty_document();
    REQUIRE(engine.play(doc).has_value());
    std::this_thread::sleep_for(700ms);

    // The user can change the preference while the transport is running, and a
    // click may well be sounding at that moment.
    engine.set_metronome_output(&second);
    std::this_thread::sleep_for(700ms);
    engine.stop();

    CHECK(clicks(first) > 0);
    CHECK(clicks(second) > 0);
    // Every click the first output was given was also released there: if the
    // move left one sounding, this is where it shows.
    CHECK(note_offs_on_channel_nine(first) == clicks(first));
    CHECK(clicks(ordinary) == 0);
}

TEST_CASE("a disabled metronome sends nothing anywhere") {
    device::MidiService midi;
    testing::RecordingOutput ordinary;
    testing::RecordingOutput click_target;
    playback::PlaybackEngine engine{midi, ordinary};
    engine.set_metronome_output(&click_target);
    // Deliberately left off: naming an output must not turn the click on.

    auto doc = empty_document();
    play_a_while(engine, doc);

    CHECK(clicks(click_target) == 0);
    CHECK(clicks(ordinary) == 0);
}

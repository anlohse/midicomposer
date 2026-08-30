#include <doctest/doctest.h>

#include "device/midi_service.hpp"
#include "music/composition.hpp"
#include "playback/playback_engine.hpp"
#include "project/project_document.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace midi_composer;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kPpqn = 480;

// One track, one note per (start, duration) pair given.
project::ProjectDocument make_document(
    const std::vector<std::pair<int64_t, int64_t>>& notes, bool muted = false) {
    music::Composition comp{base::CompositionId{1}};
    comp.set_ppqn(static_cast<int>(kPpqn));
    music::Track track{base::TrackId{1}, "Track"};
    track.set_muted(muted);
    std::uint64_t id = 1;
    for (const auto& [start, duration] : notes) {
        music::Note n;
        n.id = base::NoteId{id++};
        n.start = timeline::Tick{start};
        n.duration = timeline::TickDuration{duration};
        n.pitch = 60;
        n.velocity = 100;
        track.notes().push_back(n);
    }
    comp.tracks().push_back(std::move(track));
    return project::ProjectDocument{std::move(comp)};
}

// Records every transport transition pushed by the engine. Transitions arrive
// from both the caller's thread and the playback thread, hence the mutex.
struct StateLog {
    std::mutex mutex;
    std::vector<std::string> states;

    void install(playback::PlaybackEngine& engine) {
        engine.set_state_callback([this](playback::TransportState state, timeline::Tick) {
            std::lock_guard lock(mutex);
            states.emplace_back(playback::transport_state_name(state));
        });
    }

    std::vector<std::string> snapshot() {
        std::lock_guard lock(mutex);
        return states;
    }
};

// Polls rather than sleeping a fixed amount: the engine ticks on a 5ms loop, so
// the exact moment it notices the end is not deterministic.
bool wait_for_state(const playback::PlaybackEngine& engine, playback::TransportState want,
                    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.state() == want) return true;
        std::this_thread::sleep_for(5ms);
    }
    return engine.state() == want;
}

} // namespace

TEST_CASE("the content end is the last tick any note sounds to") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    // Not the last note by start tick — the long one underneath outlasts it, so
    // taking the back of the start-sorted list would end playback too early.
    auto doc = make_document({{0, 4 * kPpqn}, {kPpqn, kPpqn}});
    engine.refresh_snapshot(doc);
    CHECK(engine.content_end_tick() == 4 * kPpqn);

    auto empty = make_document({});
    engine.refresh_snapshot(empty);
    CHECK(engine.content_end_tick() == 0);

    // A muted track is not in the snapshot, so it cannot hold playback open.
    auto silent = make_document({{0, 4 * kPpqn}}, /*muted=*/true);
    engine.refresh_snapshot(silent);
    CHECK(engine.content_end_tick() == 0);
}

TEST_CASE("controllers and pitch bends do not hold playback open") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_document({{0, kPpqn}});
    auto& track = doc.composition().tracks().front();
    // Long after the last note. These produce no sound of their own, so waiting
    // for them would leave the transport running over silence.
    track.controller_events().push_back({base::EventId{100}, timeline::Tick{kPpqn * 50}, 7, 100});
    track.pitch_bends().push_back({base::EventId{101}, timeline::Tick{kPpqn * 60}, 4096});

    engine.refresh_snapshot(doc);
    CHECK(engine.content_end_tick() == kPpqn);
}

TEST_CASE("a composition with controllers and bends still plays and stops") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_document({{0, kPpqn / 2}});
    auto& track = doc.composition().tracks().front();
    track.controller_events().push_back({base::EventId{100}, timeline::Tick{0}, 7, 100});
    track.controller_events().push_back({base::EventId{101}, timeline::Tick{kPpqn / 4}, 10, 0});
    track.pitch_bends().push_back({base::EventId{102}, timeline::Tick{0}, -8192});
    track.pitch_bends().push_back({base::EventId{103}, timeline::Tick{kPpqn / 4}, 8191});

    // The scheduler walks these lists on every slice; the extremes are here to
    // catch an out-of-range conversion in the 14-bit bend encoding.
    engine.play(doc);
    REQUIRE(wait_for_state(engine, playback::TransportState::Stopped, 3000ms));
    CHECK(engine.current_tick().value() == 0);
}

TEST_CASE("playback stops itself once the composition runs out") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};
    StateLog log;
    log.install(engine);

    // An eighth note at the default 120bpm: 250ms of material.
    auto doc = make_document({{0, kPpqn / 2}});
    engine.play(doc);
    REQUIRE(engine.state() == playback::TransportState::Playing);

    REQUIRE(wait_for_state(engine, playback::TransportState::Stopped, 3000ms));
    // Stopping rewinds, the same as pressing stop by hand.
    CHECK(engine.current_tick().value() == 0);
    // And the UI is told, which is the only way it learns about this stop.
    CHECK(log.snapshot() == std::vector<std::string>{"playing", "stopped"});
}

TEST_CASE("an empty composition keeps running as a click track") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    // Nothing is scheduled, so "no events ahead" is true from the first tick.
    // Stopping there would make play look broken and would take the metronome
    // with it, so an empty document is left running.
    auto doc = make_document({});
    engine.play(doc);
    std::this_thread::sleep_for(150ms);
    CHECK(engine.state() == playback::TransportState::Playing);
    engine.stop();
}

TEST_CASE("recording past the end of the material keeps recording") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    // The point of recording is to wait for input, so running out of existing
    // material must not end the take.
    auto doc = make_document({{0, kPpqn / 8}});
    engine.record(doc);
    std::this_thread::sleep_for(200ms);
    CHECK(engine.state() == playback::TransportState::Recording);
    engine.stop();
}

TEST_CASE("deleting the last note while playing ends playback") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_document({{0, 200 * kPpqn}});   // minutes of material
    engine.play(doc);
    REQUIRE(engine.state() == playback::TransportState::Playing);

    // The snapshot is rebuilt on every edit while the transport is live, so the
    // end moves with the document rather than being fixed at play time.
    auto shortened = make_document({{0, kPpqn / 8}});
    engine.refresh_snapshot(shortened);
    CHECK(wait_for_state(engine, playback::TransportState::Stopped, 3000ms));
}

// ─── Mixer ───────────────────────────────────────────────────────────────────
//
// The engine sends the mixer's volume and pan as CC 7 and CC 10. Tests assert
// the snapshot it would send from, not the bytes: MidiService talks to a real
// port, there is no seam to record against, and no port is open here.

namespace {

struct TrackMix {
    std::uint8_t channel;
    std::uint8_t volume;
    std::uint8_t pan;
    bool muted = false;
    bool solo  = false;
};

// One note per track, so nothing is dropped for being empty.
project::ProjectDocument make_mix_document(const std::vector<TrackMix>& tracks) {
    music::Composition comp{base::CompositionId{1}};
    comp.set_ppqn(static_cast<int>(kPpqn));
    std::uint64_t id = 1;
    for (const auto& t : tracks) {
        music::Track track{base::TrackId{id}, "Track"};
        track.set_midi_channel(t.channel);
        track.set_volume(t.volume);
        track.set_pan(t.pan);
        track.set_muted(t.muted);
        track.set_solo(t.solo);
        music::Note n;
        n.id = base::NoteId{id++};
        n.start = timeline::Tick{0};
        n.duration = timeline::TickDuration{kPpqn};
        n.pitch = 60;
        n.velocity = 100;
        track.notes().push_back(n);
        comp.tracks().push_back(std::move(track));
    }
    return project::ProjectDocument{std::move(comp)};
}

} // namespace

TEST_CASE("a track's fader reaches its channel") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{3, 90, 20}});
    engine.refresh_snapshot(doc);

    auto mix = engine.channel_mix(3);
    REQUIRE(mix.has_value());
    CHECK(mix->volume == 90);
    CHECK(mix->pan == 20);
    // Channels no track occupies are left alone rather than defaulted, so
    // nothing is sent for them.
    CHECK_FALSE(engine.channel_mix(0).has_value());
}

TEST_CASE("a muted track leaves its channel with no mix") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{1, 100, 64, /*muted*/ true}});
    engine.refresh_snapshot(doc);

    // Nothing of a muted track plays, so sending its volume would only pin a
    // level onto a channel another track may be using.
    CHECK_FALSE(engine.channel_mix(1).has_value());
}

TEST_CASE("solo silences the mix of the tracks it excludes") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({
        {1, 100, 64, false, /*solo*/ true},
        {2,  80, 10, false, false},
    });
    engine.refresh_snapshot(doc);

    CHECK(engine.channel_mix(1).has_value());
    CHECK_FALSE(engine.channel_mix(2).has_value());
}

TEST_CASE("two tracks on one channel: the last one wins") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    // A channel has one volume. Without a rule the two would overwrite each
    // other on every refresh, so the order in the document decides.
    auto doc = make_mix_document({{5, 40, 0}, {5, 110, 127}});
    engine.refresh_snapshot(doc);

    auto mix = engine.channel_mix(5);
    REQUIRE(mix.has_value());
    CHECK(mix->volume == 110);
    CHECK(mix->pan == 127);
}

TEST_CASE("moving a fader updates the channel without a rebuild") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{2, 100, 64}});
    engine.refresh_snapshot(doc);

    engine.set_channel_mix(2, 55, 100);
    auto mix = engine.channel_mix(2);
    REQUIRE(mix.has_value());
    CHECK(mix->volume == 55);
    CHECK(mix->pan == 100);
}

TEST_CASE("a fader on a muted track does not put it back on the channel") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{4, 100, 64, /*muted*/ true}});
    engine.refresh_snapshot(doc);

    engine.set_channel_mix(4, 120, 0);
    CHECK_FALSE(engine.channel_mix(4).has_value());
}

TEST_CASE("the mix survives an edit that has nothing to do with it") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{6, 70, 30}});
    engine.refresh_snapshot(doc);
    engine.set_channel_mix(6, 45, 90);

    // A rebuild reads the document again, and the document is where the fader
    // was written; the engine's copy is not a second source of truth.
    engine.refresh_snapshot(doc);
    auto mix = engine.channel_mix(6);
    REQUIRE(mix.has_value());
    CHECK(mix->volume == 70);
}

TEST_CASE("the master scales every channel's volume") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{0, 100, 64}, {1, 60, 64}});
    doc.composition().set_master_volume(64);
    engine.refresh_snapshot(doc);

    // The faders themselves are untouched: the master is applied on the way out,
    // so pulling it down and back up returns each track to where it was set.
    CHECK(engine.channel_mix(0)->volume == 100);
    CHECK(engine.channel_mix(1)->volume == 60);
    CHECK(engine.effective_volume(0) == 50);   // 100 * 64/127, rounded
    CHECK(engine.effective_volume(1) == 30);
}

TEST_CASE("a master at full scale changes nothing") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{0, 77, 64}});
    doc.composition().set_master_volume(127);
    engine.refresh_snapshot(doc);

    // Unity has to be exact, not merely close: rounding here would move every
    // track's level the moment a master fader existed at all.
    CHECK(engine.effective_volume(0) == 77);
}

TEST_CASE("a master at zero silences every channel") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{0, 127, 64}, {4, 100, 64}});
    doc.composition().set_master_volume(0);
    engine.refresh_snapshot(doc);

    CHECK(engine.effective_volume(0) == 0);
    CHECK(engine.effective_volume(4) == 0);
}

TEST_CASE("moving the master moves the channels without a rebuild") {
    device::MidiService midi;
    playback::PlaybackEngine engine{midi};

    auto doc = make_mix_document({{2, 120, 64}});
    engine.refresh_snapshot(doc);
    CHECK(engine.effective_volume(2) == 120);

    engine.set_master_volume(32);
    CHECK(engine.effective_volume(2) == 30);   // 120 * 32/127, rounded
    CHECK(engine.channel_mix(2)->volume == 120);
}

TEST_CASE("the master defaults to unity and is saved with the project") {
    music::Composition comp{base::CompositionId{1}};
    CHECK(comp.master_volume() == 127);

    comp.set_master_volume(80);
    CHECK(comp.master_volume() == 80);

    // Out of range is clamped rather than wrapping to near-silence.
    comp.set_master_volume(200);
    CHECK(comp.master_volume() == 127);
}

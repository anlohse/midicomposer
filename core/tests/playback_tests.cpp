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

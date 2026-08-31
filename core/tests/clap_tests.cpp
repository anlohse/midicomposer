#include <doctest/doctest.h>

#include "fake_clap_plugin.hpp"
#include "playback/clap_instance.hpp"

#include <cmath>
#include <vector>

using namespace midi_composer;

namespace {

// Ready to process, the way the facade would have it.
struct Hosted {
    testing::FakeClapPlugin fake;
    playback::ClapInstance instance;

    explicit Hosted(uint32_t dialects = CLAP_NOTE_DIALECT_MIDI)
        : fake(dialects), instance(fake.plugin(), "fake", "Fake Plugin") {
        instance.set_sample_rate(32000);
        REQUIRE(instance.initialise().has_value());
        REQUIRE(instance.start().has_value());
    }

    void block(int frames, int64_t start_us = 0) {
        std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
        instance.begin_block(start_us);
        instance.render(out.data(), frames);
        last = out;
    }

    std::vector<float> last;
};

} // namespace

TEST_CASE("the host drives a plugin through its lifecycle") {
    testing::FakeClapPlugin fake;
    {
        playback::ClapInstance instance(fake.plugin(), "fake", "Fake Plugin");
        instance.set_sample_rate(44100);
        REQUIRE(instance.initialise().has_value());
        CHECK(fake.initialised);

        REQUIRE(instance.start().has_value());
        // The host owns the rate and the plugin is told, which is what having
        // several plugins on one device requires.
        CHECK(fake.activated_rate == 44100.0);
        CHECK(fake.processing);
    }
    // Destroyed on the way out rather than leaked: a plugin holds real
    // resources, and a host that forgets is a host that leaks them per project.
    CHECK(fake.destroyed);
}

TEST_CASE("a plugin that takes no note input is refused with a reason") {
    testing::FakeClapPlugin fake(0);   // neither dialect
    playback::ClapInstance instance(fake.plugin(), "fake", "Fake Plugin");

    const auto result = instance.initialise();
    REQUIRE_FALSE(result.has_value());
    // Refused at load rather than accepted and silent: an effect cannot play a
    // composition, and finding out by hearing nothing is the failure mode this
    // whole design tries to avoid.
    CHECK(result.error().code == base::ErrorCode::UnsupportedFormat);
}

TEST_CASE("a plugin that refuses to activate refuses the transport") {
    testing::FakeClapPlugin fake;
    fake.refuse_activate = true;
    playback::ClapInstance instance(fake.plugin(), "fake", "Fake Plugin");
    REQUIRE(instance.initialise().has_value());

    CHECK_FALSE(instance.start().has_value());
}

TEST_CASE("notes reach a plugin that speaks MIDI") {
    Hosted h;
    REQUIRE(h.instance.accepts_midi());

    h.instance.note_on(2, 60, 100, 0);
    h.instance.note_off(2, 60, 0);
    h.block(64);

    REQUIRE(h.fake.seen.size() == 2);
    CHECK(h.fake.seen[0].type == CLAP_EVENT_MIDI);
    CHECK(h.fake.seen[0].status == 0x92);      // note on, channel 2
    CHECK(h.fake.seen[0].key == 60);
    CHECK(h.fake.seen[0].value == 100);
    CHECK(h.fake.seen[1].status == 0x82);      // note off
}

TEST_CASE("notes reach a plugin that speaks only CLAP notes") {
    Hosted h(CLAP_NOTE_DIALECT_CLAP);
    REQUIRE(h.instance.accepts_clap_notes());
    REQUIRE_FALSE(h.instance.accepts_midi());

    h.instance.note_on(3, 64, 127, 0);
    h.block(64);

    REQUIRE(h.fake.seen.size() == 1);
    CHECK(h.fake.seen[0].type == CLAP_EVENT_NOTE_ON);
    CHECK(h.fake.seen[0].channel == 3);
    CHECK(h.fake.seen[0].key == 64);
    CHECK(h.fake.seen[0].value == 127);
}

TEST_CASE("controllers, programs and bends reach a plugin that speaks MIDI") {
    Hosted h;
    h.instance.controller(0, 7, 90, 0);
    h.instance.program_change(0, 40, 0);
    h.instance.pitch_bend(0, 4096, 0);
    h.block(64);

    REQUIRE(h.fake.seen.size() == 3);
    CHECK(h.fake.seen[0].status == 0xB0);
    CHECK(h.fake.seen[0].key == 7);
    CHECK(h.fake.seen[0].value == 90);
    CHECK(h.fake.seen[1].status == 0xC0);
    CHECK(h.fake.seen[1].key == 40);
    CHECK(h.fake.seen[2].status == 0xE0);
}

TEST_CASE("a plugin with no MIDI dialect has nowhere to put a controller") {
    Hosted h(CLAP_NOTE_DIALECT_CLAP);
    h.instance.controller(0, 7, 90, 0);
    h.block(64);

    // Reaching it through parameter automation would mean guessing which
    // parameter, and guessing wrong is worse than not sending. Counted, so the
    // silence has a reason to point at.
    CHECK(h.fake.seen.empty());
    CHECK(h.instance.dropped_events() == 1);
}

TEST_CASE("an event lands on the frame it was due") {
    Hosted h;
    // 64 frames at 32kHz is 2ms; an event due at 1ms belongs halfway in.
    h.instance.note_on(0, 60, 100, 1000);
    h.block(64, 0);

    REQUIRE(h.fake.seen.size() == 1);
    CHECK(h.fake.seen[0].time == 32);
}

TEST_CASE("an overdue event lands on the first frame rather than being dropped") {
    Hosted h;
    // Live, events routinely arrive already late: the engine sends when it
    // notices. Late has to mean now.
    h.instance.note_on(0, 60, 100, 0);
    h.block(64, 500'000);

    REQUIRE(h.fake.seen.size() == 1);
    CHECK(h.fake.seen[0].time == 0);
}

TEST_CASE("events arrive sorted by frame, as the specification requires") {
    Hosted h;
    h.instance.note_on(0, 60, 100, 1500);
    h.instance.note_on(0, 62, 100, 500);
    h.instance.note_on(0, 64, 100, 1000);
    h.block(64, 0);

    REQUIRE(h.fake.seen.size() == 3);
    CHECK(h.fake.seen[0].key == 62);
    CHECK(h.fake.seen[1].key == 64);
    CHECK(h.fake.seen[2].key == 60);
}

TEST_CASE("what the plugin renders comes back interleaved") {
    Hosted h;
    h.fake.output_level = 0.5f;
    h.block(32);

    // CLAP renders channels apart and everything downstream here is
    // interleaved, so this is the conversion, not the plugin's business.
    REQUIRE(h.last.size() == 64);
    for (float sample : h.last) CHECK(sample == doctest::Approx(0.5f));
}

TEST_CASE("a plugin that errors is reported rather than left playing silence") {
    Hosted h;
    h.fake.fail_process = true;
    h.block(32);

    REQUIRE(h.instance.failure().has_value());
    for (float sample : h.last) CHECK(sample == 0.0f);
}

TEST_CASE("nothing is rendered before the transport starts") {
    testing::FakeClapPlugin fake;
    playback::ClapInstance instance(fake.plugin(), "fake", "Fake Plugin");
    REQUIRE(instance.initialise().has_value());

    std::vector<float> out(64, 1.0f);
    instance.begin_block(0);
    instance.render(out.data(), 32);

    CHECK(fake.blocks == 0);
    for (float sample : out) CHECK(sample == 0.0f);
}

// ─── A plugin from disk ──────────────────────────────────────────────────────
//
// Skipped when there is none, rather than failing: this suite has to pass on a
// machine with no plugins installed, which is the situation the fake above
// exists for. Point CLAP_PATH at a folder to run it for real.

#include "playback/clap_library.hpp"

TEST_CASE("a real .clap loads, describes itself and plays") {
    const auto files = playback::ClapLibrary::find_plugin_files();
    if (files.empty()) {
        MESSAGE("No .clap found; set CLAP_PATH to run this against a real plugin");
        return;
    }

    const auto library = playback::ClapLibrary::open(files.front());
    REQUIRE(library.has_value());

    const auto descriptors = (*library)->plugins();
    REQUIRE_FALSE(descriptors.empty());
    MESSAGE("Loaded ", files.front(), " -> ", descriptors.front().name,
            " by ", descriptors.front().vendor);

    auto instance = (*library)->create(descriptors.front().id, 32000);
    REQUIRE(instance.has_value());
    MESSAGE("dialects: midi=", (*instance)->accepts_midi(),
            " clap=", (*instance)->accepts_clap_notes());

    REQUIRE((*instance)->start().has_value());

    // A note, then long enough for an envelope to open. What is asserted is
    // that something came out, not what it sounded like.
    (*instance)->note_on(0, 45, 100, 0);
    std::vector<float> out(512 * 2, 0.0f);
    float peak = 0.0f;
    for (int block = 0; block < 40; ++block) {
        (*instance)->begin_block(static_cast<int64_t>(block) * 16000);
        (*instance)->render(out.data(), 512);
        for (float s : out) peak = std::max(peak, std::abs(s));
    }
    (*instance)->note_off(0, 45, 0);

    CHECK_FALSE((*instance)->failure().has_value());
    CHECK(peak > 0.0f);
}

#include "device/audio_device.hpp"
#include <chrono>
#include <thread>

TEST_CASE("a real plugin can be pulled by a real audio device") {
    const auto files = playback::ClapLibrary::find_plugin_files();
    if (files.empty()) {
        MESSAGE("No .clap found; set CLAP_PATH to run this against a real plugin");
        return;
    }
    const auto library = playback::ClapLibrary::open(files.front());
    REQUIRE(library.has_value());
    const auto descriptors = (*library)->plugins();
    REQUIRE_FALSE(descriptors.empty());
    auto instance = (*library)->create(descriptors.front().id, 48000);
    REQUIRE(instance.has_value());

    // Exactly the sequence selecting it in the application performs: the device
    // opens and starts pulling before the transport has started the plugin.
    device::AudioDevice audio;
    const auto opened = audio.start(**instance);
    MESSAGE("device: ", opened.has_value() ? "opened" : opened.error().message);
    REQUIRE(opened.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(audio.frames_rendered() > 0);

    REQUIRE((*instance)->start().has_value());
    (*instance)->note_on(0, 45, 100, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    audio.stop();
    CHECK_FALSE((*instance)->failure().has_value());
}

TEST_CASE("how long a real plugin takes to answer a note") {
    const auto files = playback::ClapLibrary::find_plugin_files();
    if (files.empty()) return;
    const auto library = playback::ClapLibrary::open(files.front());
    REQUIRE(library.has_value());
    auto instance = (*library)->create((*library)->plugins().front().id, 48000);
    REQUIRE(instance.has_value());
    REQUIRE((*instance)->start().has_value());

    // Deterministic: no device, no threads. The note is due at the very first
    // frame, so whatever silence follows is the plugin's own answer time plus
    // anything this host adds.
    (*instance)->note_on(0, 45, 110, 0);

    constexpr int kBlock = 64;
    std::vector<float> out(kBlock * 2, 0.0f);
    int first_sound = -1;
    for (int block = 0; block < 1500 && first_sound < 0; ++block) {
        const int64_t when = static_cast<int64_t>(block) * kBlock * 1'000'000LL / 48000;
        (*instance)->begin_block(when);
        (*instance)->render(out.data(), kBlock);
        for (int i = 0; i < kBlock * 2; ++i) {
            if (std::abs(out[i]) > 0.001f) { first_sound = block * kBlock + i / 2; break; }
        }
    }
    REQUIRE(first_sound >= 0);
    MESSAGE("first audible sample: ", first_sound, " (",
            1000.0 * first_sound / 48000.0, " ms after the note)");
    // A plugin answering a note is not where playback latency comes from, and
    // this is the measurement that says so.
    CHECK(first_sound < 48000 / 20);   // under 50ms
}

TEST_CASE("how long a real plugin takes to activate") {
    const auto files = playback::ClapLibrary::find_plugin_files();
    if (files.empty()) return;
    const auto library = playback::ClapLibrary::open(files.front());
    REQUIRE(library.has_value());
    auto instance = (*library)->create((*library)->plugins().front().id, 48000);
    REQUIRE(instance.has_value());

    // start() activates on first use. If that took hundreds of milliseconds it
    // would delay only the plugin at the top of a transport, which is what
    // "the MIDI comes in first" would sound like.
    const auto before = std::chrono::steady_clock::now();
    REQUIRE((*instance)->start().has_value());
    const auto ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - before).count();
    MESSAGE("start() took ", ms, " ms");
    CHECK(ms < 250.0);
}

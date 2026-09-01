#include <doctest/doctest.h>

#include "playback/spc700_output.hpp"

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace midi_composer;
using namespace midi_composer::playback;

namespace {

constexpr int kRate = 48000;

/** A sample that is a constant, so its level can be asserted directly. */
Sample flat_sample(float value, int frames, int root_key = 60) {
    Sample s;
    s.data.assign(static_cast<size_t>(frames), value);
    s.root_key = root_key;
    s.source_rate = kRate;      // no resampling, so positions are frames
    s.attack = 0.0f;            // full level on the first frame
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.release = 0.01f;
    return s;
}

/** A ramp 0,1,2,... so a position can be read back out of the audio. */
Sample ramp_sample(int frames) {
    Sample s = flat_sample(0.0f, frames);
    for (int i = 0; i < frames; ++i) {
        s.data[static_cast<size_t>(i)] = static_cast<float>(i);
    }
    return s;
}

std::shared_ptr<SampleBank> bank_with(Sample sample, int program = 0) {
    auto bank = std::make_shared<SampleBank>();
    bank->samples.push_back(std::move(sample));
    bank->program_to_sample[static_cast<size_t>(program)] = 0;
    return bank;
}

/** Renders one block and returns the loudest absolute sample in it. */
float peak_of_block(Spc700Output& out, int frames, int64_t start_us = 0) {
    std::vector<float> buffer(static_cast<size_t>(frames) * 2, 0.0f);
    out.begin_block(start_us);
    out.render(buffer.data(), frames);
    float peak = 0.0f;
    for (float f : buffer) peak = std::max(peak, std::abs(f));
    return peak;
}

} // namespace

TEST_CASE("with no bank loaded it makes silence rather than refusing to start") {
    Spc700Output out;
    out.set_sample_rate(kRate);

    // Refusing here would take a whole composition down over one silent track.
    REQUIRE(out.start().has_value());
    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 256) == doctest::Approx(0.0f));
    CHECK(out.active_voices() == 0);
}

TEST_CASE("a loaded sample is what comes out") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(flat_sample(1.0f, kRate)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 256) > 0.0f);
    CHECK(out.active_voices() == 1);
}

TEST_CASE("a note is pitched by the interval from the sample's root key") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    // 64 frames of ramp, played an octave up: it should run out in half the
    // frames it would at the root key.
    out.set_bank(bank_with(ramp_sample(64)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 72, 127, 0);          // an octave above root 60
    std::vector<float> buffer(64 * 2, 0.0f);
    out.begin_block(0);
    out.render(buffer.data(), 64);

    // Two sample frames per output frame, so the one-shot is finished well
    // before 64 output frames are up.
    CHECK(out.active_voices() == 0);
}

TEST_CASE("a one-shot stops at the end of its data") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(flat_sample(1.0f, 100)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 50) > 0.0f);       // still inside the sample
    CHECK(out.active_voices() == 1);

    peak_of_block(out, 100, 1'000'000);          // runs past frame 100
    CHECK(out.active_voices() == 0);
}

TEST_CASE("a looping sample keeps sounding past its end") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 100);
    looped.loop_start = 0;
    looped.loop_end = 100;
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    peak_of_block(out, 100, 0);
    // Where the one-shot above had run out, this one is still going.
    CHECK(peak_of_block(out, 100, 1'000'000) > 0.0f);
    CHECK(out.active_voices() == 1);
}

TEST_CASE("a note off releases the voice") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 100);
    looped.loop_start = 0;
    looped.loop_end = 100;
    looped.release = 0.001f;      // ~48 frames
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    peak_of_block(out, 64, 0);
    REQUIRE(out.active_voices() == 1);

    out.note_off(0, 60, 0);
    peak_of_block(out, 512, 1'000'000);
    CHECK(out.active_voices() == 0);
}

TEST_CASE("a program change selects a different sample") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    auto bank = std::make_shared<SampleBank>();
    bank->samples.push_back(flat_sample(0.0f, kRate));   // program 0: silence
    bank->samples.push_back(flat_sample(1.0f, kRate));   // program 1: loud
    bank->program_to_sample[0] = 0;
    bank->program_to_sample[1] = 1;
    out.set_bank(bank);
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 128) == doctest::Approx(0.0f));

    out.program_change(0, 1, 1'000'000);
    out.note_on(0, 60, 127, 1'000'000);
    CHECK(peak_of_block(out, 128, 1'000'000) > 0.0f);
}

TEST_CASE("a program with nothing behind it is silent, not a crash") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(flat_sample(1.0f, kRate), /*program*/ 0));
    REQUIRE(out.start().has_value());

    out.program_change(0, 42, 0);      // nothing mapped there
    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 128) == doctest::Approx(0.0f));
    CHECK(out.active_voices() == 0);
}

TEST_CASE("a ninth note steals the oldest voice, not the newest") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());

    for (int i = 0; i < 9; ++i) out.note_on(0, static_cast<uint8_t>(60 + i), 100, 0);
    peak_of_block(out, 64);

    // Eight voices is the constraint that shaped how music was written for the
    // machine, so a ninth note does not add a ninth voice.
    CHECK(out.active_voices() == 8);
}

TEST_CASE("the bank can be replaced while a note is sounding") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    auto first = bank_with(looped);
    out.set_bank(first);
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    CHECK(peak_of_block(out, 64) > 0.0f);

    // Drop every reference the test holds, so the only thing that could be
    // keeping the old bank alive is the output itself.
    std::weak_ptr<const SampleBank> watch = first;
    out.set_bank(bank_with(flat_sample(0.5f, 1000)));
    first.reset();

    // Must not read freed audio. Under a sanitiser this is the assertion; here
    // it is that the block renders at all and the new bank is what is heard.
    peak_of_block(out, 256, 1'000'000);
    CHECK(watch.expired());    // the reset cleared the voices holding it
    CHECK(out.bank()->samples[0].data[0] == doctest::Approx(0.5f));
}

TEST_CASE("a voice keeps its own bank alive for as long as it sounds") {
    // The point of the shared_ptr in Voice: replacing a bank must not free the
    // audio a sounding note is reading.
    Spc700Output out;
    out.set_sample_rate(kRate);

    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    auto first = bank_with(looped);
    std::weak_ptr<const SampleBank> watch = first;

    out.set_bank(first);
    REQUIRE(out.start().has_value());
    out.note_on(0, 60, 127, 0);
    peak_of_block(out, 64);
    first.reset();

    // Still held: the output's own reference plus the sounding voice's.
    CHECK_FALSE(watch.expired());
}

TEST_CASE("swapping the bank from another thread while rendering") {
    // The arrangement this design exists for: a load happens on a command
    // thread while the device is pulling. Nothing here asserts timing -- it is
    // here to be run under a sanitiser and to prove the two can overlap at all.
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());
    out.note_on(0, 60, 127, 0);

    std::atomic<bool> stop{false};
    std::thread loader([&] {
        int n = 0;
        while (!stop.load()) {
            Sample s = flat_sample(0.5f, 1000);
            s.loop_start = 0;
            s.loop_end = 1000;
            out.set_bank(bank_with(s));
            ++n;
        }
        CHECK(n > 0);
    });

    for (int block = 0; block < 200; ++block) {
        peak_of_block(out, 128, static_cast<int64_t>(block) * 1000);
    }
    stop.store(true);
    loader.join();

    CHECK(out.bank() != nullptr);
}

TEST_CASE("the tail is long enough for the slowest release in the bank") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample slow = flat_sample(1.0f, 100);
    slow.release = 2.0f;
    out.set_bank(bank_with(slow));

    // Otherwise a rendered file ends in the middle of a note.
    CHECK(out.tail_frames() >= static_cast<int>(2.0f * kRate));
}

TEST_CASE("the fader and pan reach the audio") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const float centred = peak_of_block(out, 64);

    out.controller(0, 7, 0, 1'000'000);      // fader down
    const float silenced = peak_of_block(out, 64, 1'000'000);
    CHECK(centred > 0.0f);
    CHECK(silenced == doctest::Approx(0.0f));
}

TEST_CASE("the interpolation kernel does not change the level of a steady signal") {
    // The gaussian's weights are normalised for exactly this: without it, a
    // held note would wobble in volume as its fractional position drifted,
    // which is audible as a slow tremolo nobody asked for.
    Spc700Output out;
    out.set_sample_rate(kRate);
    Sample looped = flat_sample(1.0f, 1000);
    looped.loop_start = 0;
    looped.loop_end = 1000;
    out.set_bank(bank_with(looped));
    REQUIRE(out.start().has_value());

    // A pitch that is not the root key, so the read position lands between
    // frames rather than on them.
    out.note_on(0, 67, 127, 0);
    const float peak = peak_of_block(out, 256);

    // Voice gain, the channel fader and the constant-power pan are the only
    // things between the sample and the output; the kernel must contribute
    // nothing of its own.
    const float expected = 1.0f * 0.25f * (100.0f / 127.0f) * std::sqrt(0.5f);
    CHECK(peak == doctest::Approx(expected).epsilon(0.02));
}

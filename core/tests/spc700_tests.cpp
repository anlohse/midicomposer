#include <doctest/doctest.h>

#include "playback/gaussian_table.hpp"
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

TEST_CASE("the chip's interpolation table is intact") {
    // It is data now, not code: a mistyped digit would change the sound by an
    // amount nobody would trace back to here. These are the properties the
    // hardware table has, checked so an edit cannot quietly break them.
    CHECK(kGaussTable.size() == 512);

    // Unity across every fractional position. Twelve-bit quantisation costs at
    // most one count; anything further out is a transcription error.
    for (size_t p = 0; p < 256; ++p) {
        const int sum = kGaussTable[255 - p] + kGaussTable[511 - p] +
                        kGaussTable[256 + p] + kGaussTable[p];
        CAPTURE(p);
        CHECK(sum >= kGaussUnity - 1);
        CHECK(sum <= kGaussUnity + 1);
    }

    // A half bell: rising to the far end and never turning back.
    for (size_t i = 1; i < kGaussTable.size(); ++i) {
        CAPTURE(i);
        CHECK(kGaussTable[i] >= kGaussTable[i - 1]);
    }
    CHECK(kGaussTable.front() == 0);
    CHECK(kGaussTable.back() == 1305);
}

TEST_CASE("interpolating between two samples stays between them") {
    // The gaussian's outer taps can overshoot on a step, which the chip does
    // too -- but on a signal that is simply rising, the reading has to land in
    // the range the neighbours describe or the kernel is indexed backwards.
    Sample ramp;
    ramp.data.resize(64);
    for (size_t i = 0; i < ramp.data.size(); ++i) ramp.data[i] = static_cast<float>(i) / 64.0f;
    ramp.root_key = 60;
    ramp.source_rate = kRate;

    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(ramp));
    REQUIRE(out.start().has_value());

    // Read at a pitch that lands between frames, and check the output rises.
    out.note_on(0, 61, 127, 0);
    std::vector<float> buffer(48 * 2, 0.0f);
    out.begin_block(0);
    out.render(buffer.data(), 48);

    for (int i = 1; i < 40; ++i) {
        CAPTURE(i);
        CHECK(buffer[static_cast<size_t>(i) * 2] >= buffer[static_cast<size_t>(i - 1) * 2]);
    }
}

// ── The envelope's shape ─────────────────────────────────────────────────────

namespace {

/** The output level frame by frame, for one held note on a steady sample. */
std::vector<float> envelope_of(const Sample& sample, int frames, bool release_after = false) {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(sample));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    if (release_after) out.note_off(0, 60, frames * 1'000'000LL / (2 * kRate));

    std::vector<float> buffer(static_cast<size_t>(frames) * 2, 0.0f);
    out.begin_block(0);
    out.render(buffer.data(), frames);

    std::vector<float> level(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) level[static_cast<size_t>(i)] = buffer[static_cast<size_t>(i) * 2];
    return level;
}

Sample steady_sample() {
    Sample s = flat_sample(1.0f, 4000);
    s.loop_start = 0;
    s.loop_end = 4000;
    return s;
}

} // namespace

TEST_CASE("the attack rises in a straight line") {
    Sample s = steady_sample();
    s.attack = 0.02f;          // 960 frames
    s.decay = 0.0f;
    s.sustain = 1.0f;

    const auto level = envelope_of(s, 960);
    // A quarter of the way up at a quarter of the time, and so on: what
    // distinguishes a linear attack from a curved one.
    for (int fraction = 1; fraction <= 3; ++fraction) {
        const auto at = static_cast<size_t>(960 * fraction / 4);
        CAPTURE(fraction);
        CHECK(level[at] == doctest::Approx(level[959] * fraction / 4.0f).epsilon(0.05));
    }
}

TEST_CASE("the decay gives up most of its distance early") {
    Sample s = steady_sample();
    s.attack = 0.0f;
    s.decay = 0.05f;           // 2400 frames
    s.sustain = 0.0f;

    const auto level = envelope_of(s, 2400);
    const float start = level[1];

    // Exponential, so half the time is far more than half the fall. A linear
    // decay would sit at 0.5 here, which is what this used to do.
    const float halfway = level[1200] / start;
    CHECK(halfway < 0.2f);
    CHECK(halfway > 0.0f);

    // And it is still falling all the way, rather than arriving and flattening.
    CHECK(level[600] > level[1200]);
    CHECK(level[1200] > level[2000]);
}

TEST_CASE("the decay lands on the sustain level and stays there") {
    Sample s = steady_sample();
    s.attack = 0.0f;
    s.decay = 0.01f;
    s.sustain = 0.5f;

    const auto level = envelope_of(s, 4000);
    const float peak = level[1];
    // Held, not still sliding: a SoundFont sustains, and nothing here knows
    // what rate the chip would have fallen at.
    CHECK(level[2000] == doctest::Approx(peak * 0.5f).epsilon(0.05));
    CHECK(level[3900] == doctest::Approx(level[2000]).epsilon(0.01));
}

TEST_CASE("the release curves rather than sliding to zero") {
    Sample s = steady_sample();
    s.attack = 0.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.release = 0.04f;

    const auto level = envelope_of(s, 3840, /*release_after*/ true);
    const int released_at = 1920;
    const float start = level[released_at + 10];
    REQUIRE(start > 0.0f);

    // Same shape as the decay, and for the same reason: a note that slides
    // straight to zero clicks at the end.
    CHECK(level[released_at + 960] / start < 0.2f);
    CHECK(level[released_at + 960] > 0.0f);
}

TEST_CASE("a voice ends rather than fading forever") {
    // An exponential never reaches zero, so something has to decide the note
    // has stopped mattering -- otherwise every note ever played keeps a voice.
    Sample s = steady_sample();
    s.attack = 0.0f;
    s.release = 0.005f;

    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank_with(s));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    peak_of_block(out, 128, 0);
    out.note_off(0, 60, 0);
    peak_of_block(out, kRate / 4, 1'000'000);
    CHECK(out.active_voices() == 0);
}

TEST_CASE("an output names its own instruments only when it has some") {
    Spc700Output out;
    out.set_sample_rate(kRate);

    // Nothing loaded: the General MIDI names stand, which is what the UI has
    // always shown. Claiming an empty list of our own would blank the menu.
    CHECK(out.program_names().empty());

    auto bank = bank_with(flat_sample(1.0f, 100));
    bank->program_names[0] = "Grand Piano";
    out.set_bank(bank);

    const auto names = out.program_names();
    REQUIRE(names.size() == 128);
    CHECK(names[0] == "Grand Piano");
    CHECK(names[1].empty());
}

#include <doctest/doctest.h>

#include "io/sample_pitch.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace midi_composer;

namespace {

constexpr int kRate = 32000;

std::vector<float> tone(double hz, int frames, double amplitude = 0.8) {
    std::vector<float> out(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        out[static_cast<size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979 * hz * i / kRate));
    }
    return out;
}

/** A sawtooth: harmonics all the way up, which is where an autocorrelation
    detector goes wrong by locking onto one of them. */
std::vector<float> saw(double hz, int frames) {
    std::vector<float> out(static_cast<size_t>(frames));
    const double period = kRate / hz;
    for (int i = 0; i < frames; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(std::fmod(i, period) / period * 2.0 - 1.0);
    }
    return out;
}

std::vector<float> noise(int frames) {
    std::vector<float> out(static_cast<size_t>(frames));
    uint32_t state = 12345;
    for (int i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        out[static_cast<size_t>(i)] = static_cast<float>(static_cast<int32_t>(state) / 2147483648.0);
    }
    return out;
}

/** Cents between a detected frequency and the one that was asked for. */
double cents(double detected, double expected) {
    return 1200.0 * std::log2(detected / expected);
}

} // namespace

TEST_CASE("a sine's pitch is found to within a few cents") {
    for (double hz : {110.0, 220.0, 440.0, 880.0, 1760.0}) {
        const auto estimate = io::estimate_pitch(tone(hz, kRate), kRate);
        CAPTURE(hz);
        REQUIRE(estimate.frequency > 0.0);
        // A few cents is inaudible; a semitone is 100, and getting within one
        // is the whole point of interpolating the correlation peak.
        CHECK(std::abs(cents(estimate.frequency, hz)) < 10.0);
        CHECK(estimate.confidence > 0.9);
    }
}

TEST_CASE("a harmonically rich tone does not land an octave out") {
    // The classic autocorrelation failure: a signal repeating every N samples
    // also repeats every 2N, and the longer lag can correlate better.
    for (double hz : {130.81, 261.63, 523.25}) {
        const auto estimate = io::estimate_pitch(saw(hz, kRate), kRate);
        CAPTURE(hz);
        REQUIRE(estimate.frequency > 0.0);
        CHECK(std::abs(cents(estimate.frequency, hz)) < 30.0);
    }
}

TEST_CASE("the MIDI note matches the frequency, fractionally") {
    // 440Hz is A4, note 69, exactly.
    const auto a4 = io::estimate_pitch(tone(440.0, kRate), kRate);
    CHECK(a4.midi_note == doctest::Approx(69.0).epsilon(0.01));

    // And a quarter-tone above it must not round away: the fraction is what
    // becomes the sample's fine tuning.
    const auto quarter = io::estimate_pitch(tone(440.0 * std::pow(2.0, 0.5 / 12.0), kRate), kRate);
    CHECK(quarter.midi_note == doctest::Approx(69.5).epsilon(0.02));
}

TEST_CASE("noise offers no pitch rather than a made-up one") {
    const auto estimate = io::estimate_pitch(noise(kRate), kRate);
    // A drum or a cymbal ends up here, and the caller keeps its default.
    CHECK(estimate.confidence < 0.5);
}

TEST_CASE("silence offers no pitch") {
    const auto estimate = io::estimate_pitch(std::vector<float>(kRate, 0.0f), kRate);
    CHECK(estimate.frequency == 0.0);
    CHECK(estimate.confidence == 0.0);
}

TEST_CASE("a window too short to hold a period finds nothing") {
    // Two cycles of 40Hz is 1600 samples; anything less cannot show a period.
    const auto estimate = io::estimate_pitch(tone(440.0, 100), kRate);
    CHECK(estimate.frequency == 0.0);
}

TEST_CASE("the region asked for is the region measured") {
    // Half noise, half tone. Measuring the whole thing would average the two;
    // measuring the loop is why the caller passes a range at all.
    auto mixed = noise(kRate / 2);
    const auto pitched = tone(330.0, kRate / 2);
    mixed.insert(mixed.end(), pitched.begin(), pitched.end());

    const auto whole = io::estimate_pitch(mixed, kRate, 0, kRate / 2);
    CHECK(whole.confidence < 0.5);                 // the noisy half

    const auto later = io::estimate_pitch(mixed, kRate, kRate / 2, kRate);
    REQUIRE(later.frequency > 0.0);
    CHECK(std::abs(cents(later.frequency, 330.0)) < 15.0);
}

TEST_CASE("a quiet sample is measured as readily as a loud one") {
    // Level and pitch are unrelated, and normalising the correlation is what
    // keeps a quiet instrument from being read as unpitched.
    const auto loud  = io::estimate_pitch(tone(220.0, kRate, 0.9), kRate);
    const auto quiet = io::estimate_pitch(tone(220.0, kRate, 0.005), kRate);
    REQUIRE(quiet.frequency > 0.0);
    CHECK(quiet.frequency == doctest::Approx(loud.frequency).epsilon(0.001));
}

TEST_CASE("a bad range is not a crash") {
    const auto samples = tone(440.0, kRate);
    CHECK(io::estimate_pitch(samples, kRate, 500, 100).frequency == 0.0);
    CHECK(io::estimate_pitch(samples, kRate, -50, 999999).frequency > 0.0);
    CHECK(io::estimate_pitch(samples, 0).frequency == 0.0);
    CHECK(io::estimate_pitch({}, kRate).frequency == 0.0);
}

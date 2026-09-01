#include <doctest/doctest.h>

#include "playback/spc700_output.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace midi_composer;
using namespace midi_composer::playback;

namespace {

constexpr int kRate = 48000;

/** A bank of one very short click, so what comes back is unmistakably an echo
    of it rather than the note still sounding. */
std::shared_ptr<SampleBank> click_bank(const EchoSettings& echo, bool bright = false) {
    Sample s;
    s.data.assign(48, 0.0f);           // 1ms
    // Flat by default, so a peak is easy to find. `bright` alternates every
    // frame instead -- all the energy at the top of the band, which is the only
    // way a low-pass has anything to remove.
    for (size_t i = 0; i < s.data.size(); ++i) {
        s.data[i] = bright && (i % 2) ? -0.9f : 0.9f;
    }
    s.root_key = 60;
    s.source_rate = kRate;
    s.attack = 0.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.release = 0.0001f;

    auto bank = std::make_shared<SampleBank>();
    bank->samples.push_back(std::move(s));
    bank->program_to_sample[0] = 0;
    bank->echo = echo;
    return bank;
}

/** A plain delay: one tap at unity, so the echo is the signal unchanged. */
EchoSettings plain_delay(int delay_ms, float feedback, float volume) {
    EchoSettings echo;
    echo.enabled = true;
    echo.delay_ms = delay_ms;
    echo.feedback = feedback;
    echo.volume_left = volume;
    echo.volume_right = volume;
    echo.fir = {1.0f, 0, 0, 0, 0, 0, 0, 0};
    return echo;
}

/** Renders one long block and returns the left channel. */
std::vector<float> render_left(Spc700Output& out, int frames) {
    std::vector<float> buffer(static_cast<size_t>(frames) * 2, 0.0f);
    out.begin_block(0);
    out.render(buffer.data(), frames);
    std::vector<float> left(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) left[static_cast<size_t>(i)] = buffer[static_cast<size_t>(i) * 2];
    return left;
}

/** Frames where the signal rises above a threshold, as onsets. */
std::vector<int> onsets(const std::vector<float>& signal, float threshold) {
    std::vector<int> out;
    bool above = false;
    for (size_t i = 0; i < signal.size(); ++i) {
        const bool now = std::abs(signal[i]) > threshold;
        if (now && !above) out.push_back(static_cast<int>(i));
        above = now;
    }
    return out;
}

} // namespace

TEST_CASE("with no echo the output is the voices and nothing else") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(click_bank({}));       // enabled defaults to false
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const auto left = render_left(out, kRate / 4);
    CHECK(onsets(left, 0.01f).size() == 1);
}

TEST_CASE("a click comes back one delay later") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(click_bank(plain_delay(/*ms*/ 100, /*feedback*/ 0.0f, /*volume*/ 0.8f)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const auto left = render_left(out, kRate / 2);
    const auto hits = onsets(left, 0.01f);

    REQUIRE(hits.size() == 2);
    // 100ms at 48kHz. Within a frame or two: the line is read where it is about
    // to be written, so the delay is exactly its length.
    CHECK(hits[1] - hits[0] == doctest::Approx(kRate / 10).epsilon(0.001));
}

TEST_CASE("feedback repeats it, quieter each time") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(click_bank(plain_delay(50, 0.6f, 0.9f)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const auto left = render_left(out, kRate);
    const auto hits = onsets(left, 0.005f);

    REQUIRE(hits.size() >= 4);
    // Evenly spaced...
    for (size_t i = 2; i < hits.size(); ++i) {
        CHECK((hits[i] - hits[i - 1]) == doctest::Approx(kRate / 20).epsilon(0.01));
    }
    // ...and each repeat quieter than the one before it.
    float previous = 1e9f;
    for (int hit : hits) {
        float peak = 0.0f;
        for (int i = hit; i < hit + 100 && i < static_cast<int>(left.size()); ++i) {
            peak = std::max(peak, std::abs(left[static_cast<size_t>(i)]));
        }
        CHECK(peak < previous);
        previous = peak;
    }
}

TEST_CASE("the echo volume decides how loud the repeat is, not the source") {
    Spc700Output quiet_echo;
    quiet_echo.set_sample_rate(kRate);
    quiet_echo.set_bank(click_bank(plain_delay(50, 0.0f, 0.2f)));
    REQUIRE(quiet_echo.start().has_value());
    quiet_echo.note_on(0, 60, 127, 0);
    const auto a = render_left(quiet_echo, kRate / 4);

    Spc700Output loud_echo;
    loud_echo.set_sample_rate(kRate);
    loud_echo.set_bank(click_bank(plain_delay(50, 0.0f, 0.8f)));
    REQUIRE(loud_echo.start().has_value());
    loud_echo.note_on(0, 60, 127, 0);
    const auto b = render_left(loud_echo, kRate / 4);

    const int at = kRate / 20 + 10;      // just inside the repeat
    CHECK(std::abs(b[static_cast<size_t>(at)]) > std::abs(a[static_cast<size_t>(at)]) * 2.0f);
}

TEST_CASE("the taps filter what comes back") {
    // Two taps of a half each is a two-point average, which is a low-pass: it
    // cancels a signal that alternates every frame and leaves a steady one
    // alone. So the source has to alternate, or the filter has nothing to
    // remove -- averaging a constant returns the constant, which is how this
    // test first passed for the wrong reason and then failed for the right one.
    auto filtered = plain_delay(50, 0.0f, 0.9f);
    filtered.fir = {0.5f, 0.5f, 0, 0, 0, 0, 0, 0};

    Spc700Output plain;
    plain.set_sample_rate(kRate);
    plain.set_bank(click_bank(plain_delay(50, 0.0f, 0.9f), /*bright*/ true));
    REQUIRE(plain.start().has_value());
    plain.note_on(0, 60, 127, 0);
    const auto a = render_left(plain, kRate / 4);

    Spc700Output low_pass;
    low_pass.set_sample_rate(kRate);
    low_pass.set_bank(click_bank(filtered, /*bright*/ true));
    REQUIRE(low_pass.start().has_value());
    low_pass.note_on(0, 60, 127, 0);
    const auto b = render_left(low_pass, kRate / 4);

    float peak_a = 0.0f;
    float peak_b = 0.0f;
    for (size_t i = kRate / 20; i < static_cast<size_t>(kRate / 20 + 200); ++i) {
        peak_a = std::max(peak_a, std::abs(a[i]));
        peak_b = std::max(peak_b, std::abs(b[i]));
    }
    CHECK(peak_b < peak_a);
}

TEST_CASE("a runaway echo does not run away") {
    // Feedback above one, which the chip permits and a corrupt register would
    // produce. It must not grow without bound into a rendered file.
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(click_bank(plain_delay(20, 1.5f, 1.0f)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const auto left = render_left(out, kRate * 2);
    for (float f : left) {
        CHECK(std::abs(f) <= 1.0f);
        CHECK_FALSE(std::isnan(f));
    }
}

TEST_CASE("changing banks does not play the previous one's echo") {
    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(click_bank(plain_delay(200, 0.9f, 1.0f)));
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    render_left(out, kRate / 10);        // fill the line, well before it repeats

    // A different rip with a different length: what is in the line belongs to
    // the piece that is gone.
    out.set_bank(click_bank(plain_delay(60, 0.9f, 1.0f)));
    const auto left = render_left(out, kRate / 4);
    CHECK(onsets(left, 0.01f).empty());
}

TEST_CASE("the tail leaves room for the echo to decay") {
    Spc700Output dry;
    dry.set_sample_rate(kRate);
    dry.set_bank(click_bank({}));

    Spc700Output wet;
    wet.set_sample_rate(kRate);
    wet.set_bank(click_bank(plain_delay(240, 0.8f, 1.0f)));

    // Otherwise a rendered file ends by cutting the reverb off the last chord.
    CHECK(wet.tail_frames() > dry.tail_frames());
    CHECK(wet.tail_frames() > kRate);            // more than a second of it
}

TEST_CASE("an instrument that does not feed the echo still plays dry") {
    // Register $4D picks which voices reach the echo, and games use it: across
    // ninety-two rips the count runs from none to all eight. A dry lead over a
    // wet accompaniment is a mix decision, and sending everything erases it.
    auto bank = click_bank(plain_delay(100, 0.0f, 0.9f));
    bank->samples[0].echo_send = false;

    Spc700Output out;
    out.set_sample_rate(kRate);
    out.set_bank(bank);
    REQUIRE(out.start().has_value());

    out.note_on(0, 60, 127, 0);
    const auto left = render_left(out, kRate / 2);
    const auto hits = onsets(left, 0.01f);

    // Heard once, at the front, and never again.
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] < kRate / 100);
}

TEST_CASE("two instruments, one wet and one dry, share one echo") {
    // The arrangement the register exists for. Both sound; only one returns.
    auto bank = std::make_shared<SampleBank>();
    for (int i = 0; i < 2; ++i) {
        Sample s;
        s.data.assign(48, 0.9f);
        s.root_key = 60;
        s.source_rate = kRate;
        s.attack = 0.0f;
        s.decay = 0.0f;
        s.sustain = 1.0f;
        s.release = 0.0001f;
        s.echo_send = (i == 0);
        bank->samples.push_back(std::move(s));
        bank->program_to_sample[i] = i;
    }
    bank->echo = plain_delay(100, 0.0f, 0.9f);

    Spc700Output wet;
    wet.set_sample_rate(kRate);
    wet.set_bank(bank);
    REQUIRE(wet.start().has_value());
    wet.program_change(0, 0, 0);
    wet.note_on(0, 60, 127, 0);
    CHECK(onsets(render_left(wet, kRate / 2), 0.01f).size() == 2);

    Spc700Output dry;
    dry.set_sample_rate(kRate);
    dry.set_bank(bank);
    REQUIRE(dry.start().has_value());
    dry.program_change(0, 1, 0);
    dry.note_on(0, 60, 127, 0);
    CHECK(onsets(render_left(dry, kRate / 2), 0.01f).size() == 1);
}

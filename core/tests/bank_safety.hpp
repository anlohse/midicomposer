#pragma once

#include "playback/sample_bank.hpp"
#include "playback/spc700_output.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace midi_composer::testing {

/**
 * What a bank has to satisfy before it is allowed near the audio thread.
 *
 * Both loaders read files somebody else wrote, and a bank is the only thing
 * standing between those bytes and a real-time thread that indexes arrays with
 * what it finds there. A zone pointing past the end of the samples is a read
 * out of bounds; a sample rate of zero stalls a voice on one frame forever; a
 * negative one walks it backwards. Checked here rather than in each loader's
 * own tests, because it is the same contract whichever format it came from.
 */
inline void must_be_safe_to_play(const playback::SampleBank& bank) {
    for (const auto& program : bank.programs) {
        for (const auto& zone : program.zones) {
            REQUIRE(zone.sample >= 0);
            REQUIRE(zone.sample < static_cast<int>(bank.samples.size()));
            const auto& audio = bank.samples[static_cast<size_t>(zone.sample)];
            REQUIRE_FALSE(audio.data.empty());
            REQUIRE(audio.source_rate > 0);
            REQUIRE(zone.low_key <= zone.high_key);
            REQUIRE(zone.low_velocity <= zone.high_velocity);
            if (zone.loop_start >= 0) {
                REQUIRE(zone.loop_start < static_cast<int>(audio.data.size()));
                REQUIRE(zone.loop_end > zone.loop_start);
                REQUIRE(zone.loop_end <= static_cast<int>(audio.data.size()));
            }
            REQUIRE(zone.root_key >= 0);
            REQUIRE(zone.root_key <= 127);
        }
    }
}

/**
 * Plays a bank briefly, which is where a bad index would actually bite.
 *
 * Every program, and the ends of the keyboard as well as the middle: a zone is
 * read fastest at the top and its loop hardest at the bottom. The samples are
 * summarised into two facts rather than asserted one by one -- an assertion per
 * frame turns this into a hundred million of them and says nothing more.
 */
inline void play_briefly(std::shared_ptr<const playback::SampleBank> bank) {
    playback::Spc700Output out;
    out.set_sample_rate(48000);
    out.set_bank(std::move(bank));
    REQUIRE(out.start().has_value());

    std::vector<float> buffer(128 * 2, 0.0f);
    bool all_finite = true;
    float loudest = 0.0f;
    for (int program = 0; program < 128; ++program) {
        out.program_change(0, static_cast<uint8_t>(program), 0);
        for (uint8_t key : {uint8_t{0}, uint8_t{60}, uint8_t{127}}) {
            out.note_on(0, key, 100, 0);
            for (int block = 0; block < 3; ++block) {
                out.begin_block(static_cast<int64_t>(block) * 2666);
                out.render(buffer.data(), 128);
                for (float f : buffer) {
                    if (!std::isfinite(f)) all_finite = false;
                    loudest = std::max(loudest, std::abs(f));
                }
            }
            out.note_off(0, key, 0);
        }
    }
    // A NaN would spread through the mixer into the device, and nothing
    // downstream would say where it came from.
    CHECK(all_finite);
    CHECK(loudest <= 1.0f);
    CHECK(out.dropped_events() == 0);
}

/**
 * One of three kinds of damage, chosen by the caller so a run covers all of
 * them evenly.
 *
 * They break different things: a flipped byte lies about a count, a truncation
 * ends a structure mid-record, and a smashed run destroys a whole header.
 * Mutation rather than random bytes, because a file that is almost valid
 * reaches far deeper into a parser than noise does -- noise is rejected by the
 * first four bytes.
 */
inline std::string damaged(const std::string& good, std::mt19937& rng, int kind) {
    std::string bytes = good;
    if (bytes.empty()) return bytes;
    switch (kind % 3) {
        case 0:
            bytes[rng() % bytes.size()] = static_cast<char>(rng() & 0xFF);
            break;
        case 1:
            bytes.resize(rng() % bytes.size());
            break;
        default: {
            const size_t at = rng() % bytes.size();
            const size_t span = std::min<size_t>(16, bytes.size() - at);
            for (size_t i = 0; i < span; ++i) bytes[at + i] = static_cast<char>(rng() & 0xFF);
            break;
        }
    }
    return bytes;
}

} // namespace midi_composer::testing

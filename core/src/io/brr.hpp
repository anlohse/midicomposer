#pragma once

#include <cstdint>
#include <vector>

namespace midi_composer::io {

/**
 * BRR: the SNES's sample format.
 *
 * Nine bytes carry sixteen samples. One header byte gives a shift, one of four
 * predictors, and two flags; the other eight hold sixteen signed nibbles, each
 * a *correction* to what the predictor already guessed. That is the whole
 * format, and it is why a 64KB sound chip could hold an orchestra: roughly 4.5
 * bits a sample, with the error absorbed by a filter rather than by more bits.
 *
 * ── On fidelity, stated plainly ──────────────────────────────────────────────
 *
 * The predictors here are the documented coefficients -- 15/16 for filter 1,
 * 61/32 and 15/16 for filter 2, 115/64 and 13/16 for filter 3 -- written as the
 * shifts the hardware uses. The tests check this decoder against those
 * coefficients, which is not the same as checking it against a console. Without
 * a reference rip to compare with, "bit-exact" is a claim this cannot make, and
 * saying so is better than implying otherwise.
 */
struct BrrDecoded {
    std::vector<float> samples;   // normalised to [-1, 1]

    /** Where the block flagged as the loop point begins, in samples, or -1.
        A ripped sample carries its loop address separately; this is the flag
        the blocks themselves set. */
    int loop_start{-1};

    /** Whether an end-flagged block was reached. A run of BRR with no end flag
        is a sample that runs into whatever memory follows it. */
    bool ended{false};
};

/**
 * Decode BRR blocks until an end flag or the data runs out.
 *
 * `loop_offset` is the byte offset within `data` that the sample's directory
 * entry named as its loop point, or -1; it is translated into a sample index in
 * the result. Decoding is bounded by `data.size()`, so a sample whose end flag
 * was lost cannot run past the buffer.
 */
BrrDecoded decode_brr(const std::vector<uint8_t>& data, size_t start_offset,
                      int loop_offset = -1);

} // namespace midi_composer::io

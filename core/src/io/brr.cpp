#include "brr.hpp"

#include <algorithm>

namespace midi_composer::io {

namespace {

constexpr size_t kBlockBytes = 9;
constexpr int kSamplesPerBlock = 16;

/**
 * The four predictors, as the hardware's shifts.
 *
 * `p1` and `p2` are the two previously decoded samples. What each one computes:
 *
 *   0:  nothing -- the nibble is the sample
 *   1:  p1 * 15/16
 *   2:  p1 * 61/32  -  p2 * 15/16
 *   3:  p1 * 115/64 -  p2 * 13/16
 *
 * Written as shifts rather than as multiplies by those fractions because that
 * is the arithmetic the chip does, and the rounding of a right shift on a
 * negative number is part of the result.
 */
int predict(int filter, int p1, int p2) {
    switch (filter) {
        case 0:
            return 0;
        case 1:
            return p1 + ((-p1) >> 4);
        case 2:
            return (p1 << 1) + ((-((p1 << 1) + p1)) >> 5) - p2 + (p2 >> 4);
        case 3:
        default:
            return (p1 << 1) + ((-(p1 * 13)) >> 6) - p2 + ((p2 * 3) >> 4);
    }
}

/** Clamp to 16 bits, then wrap into 15. The chip's accumulator is 15-bit and
    overflowing it wraps rather than saturates, which is audible as a crack on
    a sample that was encoded too hot. */
int clamp_and_wrap(int value) {
    value = std::clamp(value, -32768, 32767);
    // Sign-extend the low 15 bits.
    value &= 0x7FFF;
    if (value & 0x4000) value -= 0x8000;
    return value;
}

} // namespace

BrrDecoded decode_brr(const std::vector<uint8_t>& data, size_t start_offset, int loop_offset) {
    BrrDecoded out;
    if (start_offset >= data.size()) return out;

    int p1 = 0;
    int p2 = 0;
    size_t at = start_offset;

    while (at + kBlockBytes <= data.size()) {
        if (loop_offset >= 0 && out.loop_start < 0 &&
            static_cast<size_t>(loop_offset) == at) {
            out.loop_start = static_cast<int>(out.samples.size());
        }

        const uint8_t header = data[at];
        const int range  = header >> 4;
        const int filter = (header >> 2) & 3;
        const bool end   = (header & 1) != 0;

        for (int i = 0; i < kSamplesPerBlock; ++i) {
            const uint8_t byte = data[at + 1 + static_cast<size_t>(i / 2)];
            int nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
            if (nibble > 7) nibble -= 16;              // signed 4-bit

            int sample;
            if (range <= 12) {
                sample = (nibble << range) >> 1;
            } else {
                // Ranges 13 to 15 do not shift further; they collapse to the
                // sign. Encoders do not emit them, but a rip of arbitrary
                // memory can contain anything.
                sample = nibble < 0 ? -2048 : 0;
            }

            sample = clamp_and_wrap(sample + predict(filter, p1, p2));
            p2 = p1;
            p1 = sample;

            // The accumulator is 15-bit; the DSP presents it doubled.
            out.samples.push_back(static_cast<float>(sample) / 16384.0f);
        }

        at += kBlockBytes;
        if (end) { out.ended = true; break; }
    }

    // A loop that pointed past everything decoded is no loop at all rather than
    // an index nothing can use.
    if (out.loop_start >= static_cast<int>(out.samples.size())) out.loop_start = -1;
    return out;
}

} // namespace midi_composer::io

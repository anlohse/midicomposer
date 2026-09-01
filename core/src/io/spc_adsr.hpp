#pragma once

#include <array>

namespace midi_composer::io {

/**
 * The S-DSP's envelope rate tables.
 *
 * ── Where these come from ────────────────────────────────────────────────────
 *
 * Sony's own APU documentation, transcribed on the Super Famicom Development
 * Wiki as "Table 2.2 ADSR Parameters":
 *
 *     https://wiki.superfamicom.org/spc700-reference
 *
 * They are published as *times*, not as the counter periods an emulator works
 * in, which happens to be the more useful form here: the envelope in
 * `Spc700Output` is time-based, and a SoundFont states times too, so a rip and
 * a SoundFont end up speaking the same language.
 *
 * ── Why they were once said to have no consumer ──────────────────────────────
 *
 * This file exists because that claim was wrong. The reasoning went: an `.spc`
 * is a snapshot, so its per-voice ADSR registers describe eight voices at one
 * instant rather than a table of instruments. True as far as it goes -- and it
 * skipped register `$x4`, SRCN, which says *which sample* each voice is set to
 * play. With that, the eight voices are eight (sample, envelope) pairs written
 * by the game itself.
 *
 * Measured across ninety-two Chrono Trigger rips: every file carries an
 * envelope for between two and eight distinct samples, mean 4.9, in 76 distinct
 * combinations -- varied per-instrument settings, not one default repeated. The
 * samples they point at are overwhelmingly the long ones, which are the real
 * instruments rather than the percussion hits.
 *
 * So a rip gives real envelopes for about a fifth of its samples. The rest keep
 * defaults, which is a worse answer than the game's and a much better one than
 * an invented envelope for all of them.
 */

/** AR 0..15: time from silence to full. 15 is instant. */
inline constexpr std::array<float, 16> kAttackSeconds = {
    4.1f,   2.5f,   1.5f,   1.0f,
    0.640f, 0.380f, 0.260f, 0.160f,
    0.096f, 0.064f, 0.040f, 0.024f,
    0.016f, 0.010f, 0.006f, 0.0f,
};

/** DR 0..7: time from full down to the sustain level. */
inline constexpr std::array<float, 8> kDecaySeconds = {
    1.2f,   0.740f, 0.440f, 0.290f,
    0.180f, 0.110f, 0.074f, 0.037f,
};

/** SL 0..7: the level decay stops at, in eighths. 7 is full, so decay does
    nothing -- which is how a driver asks for a note that simply holds. */
inline constexpr std::array<float, 8> kSustainLevel = {
    1.0f / 8, 2.0f / 8, 3.0f / 8, 4.0f / 8,
    5.0f / 8, 6.0f / 8, 7.0f / 8, 8.0f / 8,
};

/** SR 0..31: time from full to silence while the key is still held. Index 0 is
    the chip's "infinite", represented here as zero seconds, which is what
    `Sample::sustain_rate` means by "hold". */
inline constexpr std::array<float, 32> kSustainSeconds = {
    0.0f,   38.0f,  28.0f,  24.0f,
    19.0f,  14.0f,  12.0f,  9.4f,
    7.1f,   5.9f,   4.7f,   3.5f,
    2.9f,   2.4f,   1.8f,   1.5f,
    1.2f,   0.880f, 0.740f, 0.590f,
    0.440f, 0.370f, 0.290f, 0.220f,
    0.180f, 0.150f, 0.110f, 0.092f,
    0.074f, 0.055f, 0.037f, 0.018f,
};

/** One voice's envelope registers, decoded. */
struct AdsrRegisters {
    bool  enabled{false};   // ADSR1 bit 7; when clear the voice uses GAIN
    float attack{0.0f};
    float decay{0.0f};
    float sustain{1.0f};
    float sustain_rate{0.0f};
};

/**
 * Decode registers `$x5` and `$x6`.
 *
 * ADSR1: bit 7 enables ADSR, bits 6-4 are DR, bits 3-0 are AR.
 * ADSR2: bits 7-5 are SL, bits 4-0 are SR.
 */
inline AdsrRegisters decode_adsr(uint8_t adsr1, uint8_t adsr2) {
    AdsrRegisters out;
    out.enabled = (adsr1 & 0x80) != 0;
    if (!out.enabled) return out;
    out.attack       = kAttackSeconds[adsr1 & 0x0F];
    out.decay        = kDecaySeconds[(adsr1 >> 4) & 0x07];
    out.sustain      = kSustainLevel[(adsr2 >> 5) & 0x07];
    out.sustain_rate = kSustainSeconds[adsr2 & 0x1F];
    return out;
}

} // namespace midi_composer::io

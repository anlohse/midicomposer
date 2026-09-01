#pragma once

#include <cstddef>
#include <vector>

namespace midi_composer::io {

/**
 * What note a recorded sample sounds at, measured from the audio itself.
 *
 * ── Why this has to exist ────────────────────────────────────────────────────
 *
 * A SoundFont states the pitch each sample was recorded at. A rip cannot: the
 * `.spc` holds the audio and nothing that says what it is, because the game's
 * driver knew and the driver is not in the file. Without an answer every
 * instrument in a rip is transposed by its own arbitrary interval, which is the
 * single most audible thing wrong with a ripped bank -- worse than any filter,
 * because it is wrong per instrument rather than wrong overall.
 *
 * The audio can be asked instead. A pitched instrument's loop is periodic --
 * that is what makes it loopable -- so the period is measurable, and the period
 * is the pitch.
 *
 * ── What it cannot do ────────────────────────────────────────────────────────
 *
 * A drum, a cymbal or a noise sample has no fundamental to find, and no answer
 * for one would be an answer. That is what `confidence` is for: below a
 * threshold the caller should keep its default rather than transpose an
 * instrument by a number this made up.
 */
struct PitchEstimate {
    /** Hz, at the rate the samples were recorded. Zero when nothing was found. */
    double frequency{0.0};

    /** How periodic the window actually was, 0 to 1. A sustained tone lands
        near 1; noise and percussion stay low. */
    double confidence{0.0};

    /** The fractional MIDI note number for `frequency`, or 0 when there is
        none. Fractional on purpose: rounding it away is the difference between
        an instrument that is in tune and one that is thirty cents sharp. */
    double midi_note{0.0};
};

/**
 * Estimate over `[from, to)`, or over a sensible part of the whole when `to` is
 * negative.
 *
 * Prefer to pass the loop region: an instrument's attack is inharmonic and its
 * decay is quiet, while the loop is the steady state the whole design of a
 * looped sample exists to reach.
 */
PitchEstimate estimate_pitch(const std::vector<float>& samples, int sample_rate,
                             int from = 0, int to = -1);

} // namespace midi_composer::io

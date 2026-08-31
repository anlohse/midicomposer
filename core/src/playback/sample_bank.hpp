#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace midi_composer::playback {

/**
 * Sampled instruments, in the shape a voice needs to play them.
 *
 * Deliberately not any file format's shape. A SoundFont preset and a sample
 * ripped out of an `.spc` describe an instrument in very different terms, and a
 * bank that mirrored either would make the other a translation of a translation.
 * What a voice actually needs is short: where the audio is, where it loops, what
 * pitch it was recorded at, and how it fades.
 *
 * Immutable once built. That is what makes a bank swappable while sound is
 * playing: the audio thread holds a `shared_ptr<const SampleBank>` for the
 * length of a block and nothing can change underneath it. Loading a new one
 * builds a new one.
 */
struct Sample {
    /** Mono, normalised to [-1, 1]. Interleaving is the mixer's business and
        stereo instruments are not modelled yet. */
    std::vector<float> data;

    /** Frames. `loop_start < 0` means the sample plays once and stops. */
    int loop_start{-1};
    int loop_end{0};

    /** The MIDI note the recording sounds at, plus a correction in cents. */
    int    root_key{60};
    double fine_tune_cents{0.0};

    /** The rate the audio was recorded at, which is rarely the host's. */
    int source_rate{32000};

    /** Seconds. A plain four-stage envelope: the chip's rate table is not
        reproduced here, so these are times rather than chip rates. */
    float attack{0.002f};
    float decay{0.0f};
    float sustain{1.0f};        // level, 0..1
    float release{0.08f};

    std::string name;
};

/**
 * A set of samples plus the mapping a program change needs.
 *
 * One sample per program rather than key-ranged layers: a layered instrument is
 * a real thing and this is not it yet, but the mapping is the part that has to
 * exist for a composition to select anything at all.
 */
struct SampleBank {
    std::vector<Sample> samples;

    /** Index into `samples`, or -1 for a program with nothing behind it. */
    std::array<int, 128> program_to_sample{};

    /** What to call this in the UI -- the file's own name, usually. */
    std::string name;

    SampleBank() { program_to_sample.fill(-1); }

    [[nodiscard]] const Sample* for_program(int program) const {
        if (program < 0 || program > 127) return nullptr;
        const int index = program_to_sample[static_cast<size_t>(program)];
        if (index < 0 || index >= static_cast<int>(samples.size())) return nullptr;
        return &samples[static_cast<size_t>(index)];
    }

    /** Whether anything can be played out of this at all. */
    [[nodiscard]] bool empty() const { return samples.empty(); }
};

} // namespace midi_composer::playback

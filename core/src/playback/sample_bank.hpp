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

    /** Seconds, and a level for sustain. The chip states these as register
        values and the loaders convert; times are the common language because a
        SoundFont states times too. */
    float attack{0.002f};
    float decay{0.0f};
    float sustain{1.0f};        // level, 0..1

    /**
     * How long a held note takes to fade from the sustain level to silence.
     *
     * Zero means it holds indefinitely, which is what a SoundFont means and
     * what most instruments want. The SNES has no such thing -- its sustain
     * *rate* always decays, and a driver picks "infinite" by setting rate zero
     * -- so a rip fills this in and a SoundFont leaves it alone.
     */
    float sustain_rate{0.0f};

    float release{0.08f};

    /**
     * Whether this instrument reaches the echo at all.
     *
     * The chip decides per voice, with register `$4D`, and games use it: across
     * ninety-two rips the count runs from none to all eight, mean five. A lead
     * dry against a wet accompaniment is a mix decision somebody made, and
     * sending everything flattens it.
     *
     * True by default, which is what a SoundFont means by saying nothing.
     */
    bool echo_send{true};

    std::string name;
};

/**
 * The chip's echo, as a game set it up.
 *
 * Unlike the interpolation kernel and the ADSR rates, none of this is hardware
 * data we would have to know: it is *per game*, and it sits in the DSP
 * registers of any `.spc`. The echo a piece was written with is therefore
 * recoverable from the rip, taps and all, which is why this travels with the
 * bank rather than being a setting of the output.
 *
 * The eight FIR taps are the part that gives a game's echo its character --
 * most set them to a gentle low-pass so repeats get darker rather than merely
 * quieter, which is a large part of why SNES reverb sounds like a room instead
 * of a delay pedal.
 */
struct EchoSettings {
    bool enabled{false};

    /** How far back the line reads, in milliseconds. The chip allows 0 to 240
        in steps of 16; this is whatever the rip declared. */
    int delay_ms{0};

    /** How much of the filtered echo is written back in. Near 1 the line never
        decays, which the chip permits and a game does not use. */
    float feedback{0.0f};

    /** How much of the echo reaches the output, per side. */
    float volume_left{0.0f};
    float volume_right{0.0f};

    /** Eight taps applied across the delayed signal, oldest last. */
    std::array<float, 8> fir{};
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

    /**
     * What each program is called, for the instrument list.
     *
     * A program has a name; a sample is data. Keeping them apart matters
     * because one sample can be reached by several programs, and because a rip
     * has no names at all -- what it has is measurements, which make a better
     * label than a number does.
     *
     * Empty means the slot has nothing in it.
     */
    std::array<std::string, 128> program_names{};

    /** What to call this in the UI -- the file's own name, usually. */
    std::string name;

    /** The echo the rip was playing through, when there was one. */
    EchoSettings echo;

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

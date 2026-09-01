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
 *
 * Immutable once built. That is what makes a bank swappable while sound is
 * playing: the audio thread holds a `shared_ptr<const SampleBank>` for the
 * length of a block and nothing can change underneath it. Loading a new one
 * builds a new one.
 *
 * ── Audio and how to play it are separate ────────────────────────────────────
 *
 * `Sample` is recorded sound and nothing else. Everything about *how* to play it
 * -- what note it sounds at, where it loops, how it fades -- lives on a `Zone`,
 * because those answers are not properties of the audio. A SoundFont says so
 * plainly: one recording is reached by several zones covering different parts of
 * the keyboard, each with its own root key and envelope. A piano sampled at
 * three octaves is three recordings and a dozen zones.
 *
 * The first version of this fused the two, which worked exactly as long as every
 * instrument was one recording stretched across the keyboard.
 */
struct Sample {
    /** Mono, normalised to [-1, 1]. Interleaving is the mixer's business and
        stereo instruments are not modelled yet. */
    std::vector<float> data;

    /** The rate the audio was recorded at, which is rarely the host's. */
    int source_rate{32000};

    std::string name;
};

/**
 * One recording, played one way, over one part of the keyboard.
 *
 * A program is a list of these, and a note starts every one whose ranges
 * contain it -- usually exactly one, sometimes several stacked on purpose. See
 * `SampleBank::zones_for`.
 */
struct Zone {
    /** Index into `SampleBank::samples`. */
    int sample{-1};

    /** Inclusive, and the whole keyboard by default so a single-zone
        instrument needs no ranges at all. */
    uint8_t low_key{0};
    uint8_t high_key{127};
    uint8_t low_velocity{0};
    uint8_t high_velocity{127};

    /** Frames into the sample. `loop_start < 0` means it plays once and stops. */
    int loop_start{-1};
    int loop_end{0};

    /** The MIDI note this zone sounds at when played at the recording's own
        rate, plus a correction in cents. */
    int    root_key{60};
    double fine_tune_cents{0.0};

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
     * How loud this zone is, 1.0 being the recording as it stands.
     *
     * A SoundFont states an attenuation per zone and expects it applied. It
     * matters most where zones stack: a preset that layers two instruments
     * would otherwise arrive at twice the level of a plain one, and a bank
     * that layers on purpose sets the attenuation that pays for it.
     *
     * One, meaning untouched, is what a rip means -- the chip has a per-voice
     * volume, but a snapshot's value belongs to the note that voice was
     * playing rather than to the sample.
     */
    float gain{1.0f};

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
};

/** What a program change selects. */
struct Program {
    /** Shown in the instrument list. Empty means the program has nothing. */
    std::string name;

    /** Empty when the program has nothing behind it. */
    std::vector<Zone> zones;
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

struct SampleBank {
    /** The recordings. Zones index into this, and several may share one. */
    std::vector<Sample> samples;

    std::array<Program, 128> programs{};

    /** What to call this in the UI -- the file's own name, usually. */
    std::string name;

    /** The echo the rip was playing through, when there was one. */
    EchoSettings echo;

    /**
     * The zone that should play `key` at `velocity`, or null.
     *
     * First match wins rather than best match: a SoundFont's zones are meant to
     * partition the keyboard, and where they overlap the format says the first
     * applies. Ranking them would invent a rule the file did not state.
     */
    /**
     * Every zone a note starts, not just the first one that fits.
     *
     * Zones stack. A preset may name several instruments to sound together,
     * and a single instrument may cover one key and velocity twice -- across
     * the 128 programs of a General MIDI bank ripped from SNES games, 19 do,
     * costing up to four voices for one note. Taking the first match silently
     * drops the rest, which is not a thinner sound so much as a different one:
     * the layer that was dropped is often the body under a transient.
     *
     * Writes into storage the caller owns and returns how many it filled. This
     * runs on the audio thread, where a vector would be an allocation, and the
     * ceiling is the caller's: a note cannot cost more voices than the chip
     * has, so passing the voice count is the natural bound. Zones past it are
     * dropped here rather than left to steal each other.
     */
    [[nodiscard]] int zones_for(int program, int key, int velocity, const Zone** out,
                                int capacity) const {
        if (program < 0 || program > 127 || !out || capacity <= 0) return 0;
        int found = 0;
        for (const auto& zone : programs[static_cast<size_t>(program)].zones) {
            if (key < zone.low_key || key > zone.high_key) continue;
            if (velocity < zone.low_velocity || velocity > zone.high_velocity) continue;
            if (zone.sample < 0 || zone.sample >= static_cast<int>(samples.size())) continue;
            out[found++] = &zone;
            if (found == capacity) break;
        }
        return found;
    }

    /** The first zone a note would start. `zones_for` is what plays it; this is
        for asking what a program does with a key without meaning to sound it. */
    [[nodiscard]] const Zone* zone_for(int program, int key, int velocity) const {
        if (program < 0 || program > 127) return nullptr;
        for (const auto& zone : programs[static_cast<size_t>(program)].zones) {
            if (key < zone.low_key || key > zone.high_key) continue;
            if (velocity < zone.low_velocity || velocity > zone.high_velocity) continue;
            if (zone.sample < 0 || zone.sample >= static_cast<int>(samples.size())) continue;
            return &zone;
        }
        return nullptr;
    }

    /** Whether a program has anything behind it at all, regardless of where on
        the keyboard. Used for the instrument list. */
    [[nodiscard]] bool has_program(int program) const {
        return program >= 0 && program <= 127 &&
               !programs[static_cast<size_t>(program)].zones.empty();
    }

    [[nodiscard]] const Sample* sample_of(const Zone& zone) const {
        if (zone.sample < 0 || zone.sample >= static_cast<int>(samples.size())) return nullptr;
        return &samples[static_cast<size_t>(zone.sample)];
    }

    /** Whether anything can be played out of this at all. */
    [[nodiscard]] bool empty() const { return samples.empty(); }
};

} // namespace midi_composer::playback

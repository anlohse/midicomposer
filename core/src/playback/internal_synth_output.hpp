#pragma once

#include "playback/output_plugin.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace midi_composer::playback {

/**
 * An output that makes the sound itself, instead of handing MIDI to something
 * that does.
 *
 * It is here to be the second implementation. An interface designed against one
 * example abstracts the wrong things, and everything that was only asserted
 * about OutputPlugin until now -- that a plugin can be something other than a
 * MIDI port, that the timestamps are usable, that configuration can be declared
 * -- is either true of this one or it was never true.
 *
 * ── What it models, and what it does not ─────────────────────────────────────
 *
 * The voice chain is shaped after the SNES S-DSP, which is the direction this
 * was always heading: eight voices at 32kHz, sample playback driven by a pitch
 * *rate* rather than a frequency, per-voice left/right levels, an envelope per
 * voice, and voice stealing when a ninth note arrives.
 *
 * It is deliberately not called an SPC-700. The chip's character lives in three
 * tables -- the gaussian interpolation kernel, the ADSR rate table, and BRR
 * sample decoding -- and reproducing those from memory would mean inventing
 * numbers that look authentic and are not. Four-point Hermite interpolation and
 * a plain millisecond envelope stand in for the first two; the third needs
 * sample data to decode, which is the open question about where a bank comes
 * from. A real SPC-700 plugin is those three things added to this shape.
 *
 * The waveforms are generated in code rather than shipped, which keeps the
 * question of whose samples these are from arising at all.
 */
class InternalSynthOutput final : public OutputPlugin, public AudioSource {
public:
    InternalSynthOutput();

    [[nodiscard]] std::string_view id() const override { return "internal-synth"; }
    [[nodiscard]] std::string_view name() const override { return "Internal Synth"; }

    [[nodiscard]] AudioSource* audio() override { return this; }

    // ── Configuration ────────────────────────────────────────────────────────

    [[nodiscard]] std::vector<Parameter> parameters() const override;
    [[nodiscard]] ParameterValue get_parameter(std::string_view name) const override;
    base::Result<void> set_parameter(std::string_view name, const ParameterValue& value) override;

    // ── OutputPlugin ─────────────────────────────────────────────────────────

    base::Result<void> start() override;
    void stop() override;

    void note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) override;
    void note_off(uint8_t channel, uint8_t pitch, int64_t when_us) override;
    void controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us) override;
    void program_change(uint8_t channel, uint8_t program, int64_t when_us) override;
    void pitch_bend(uint8_t channel, int16_t value, int64_t when_us) override;

    [[nodiscard]] std::optional<base::Error> failure() const override { return std::nullopt; }

    // ── AudioSource ──────────────────────────────────────────────────────────

    [[nodiscard]] int sample_rate() const override { return kSampleRate; }
    void prepare_render(int64_t start_us) override;
    void render(float* interleaved, int frames) override;
    [[nodiscard]] int tail_frames() const override;

    /** Voices sounding right now. For assertions about stealing. */
    [[nodiscard]] int active_voices() const;

private:
    // The S-DSP's rate, and its polyphony. Eight is a limit worth keeping: it is
    // the constraint that shaped how music was written for the machine.
    static constexpr int kSampleRate = 32000;
    static constexpr int kVoices     = 8;
    static constexpr int kChannels   = 16;
    static constexpr int kWaveLength = 256;

    enum class Waveform { Saw, Square, Triangle, Noise };

    struct Event {
        enum class Kind { NoteOn, NoteOff, Controller, PitchBend };
        Kind    kind;
        uint8_t channel;
        int     a;
        int     b;
        int64_t when_us;
    };

    struct Voice {
        bool     active{false};
        uint8_t  channel{0};
        uint8_t  pitch{0};
        double   phase{0.0};      // position in the wavetable
        double   rate{1.0};       // wavetable samples per output frame
        float    level{0.0f};     // velocity
        float    envelope{0.0f};
        bool     releasing{false};
        uint64_t started{0};      // for stealing the oldest
    };

    struct Channel {
        float volume{100.0f / 127.0f};
        float pan{0.5f};          // 0 left, 1 right
        float bend{0.0f};         // semitones
    };

    void   apply(const Event& e);
    void   start_note(uint8_t channel, uint8_t pitch, uint8_t velocity);
    void   release_note(uint8_t channel, uint8_t pitch);
    double rate_for(uint8_t pitch, float bend) const;
    float  sample_at(double phase) const;
    void   rebuild_wave();

    mutable std::mutex m_mutex;   // events arrive on one thread, render on another

    std::vector<Event> m_pending;
    std::array<Voice, kVoices>     m_voices{};
    std::array<Channel, kChannels> m_channels{};
    std::vector<float> m_wave;
    Waveform m_waveform{Waveform::Saw};

    int64_t  m_now_us{0};         // start of the block being rendered
    uint64_t m_age{0};            // monotonic, for voice stealing
};

} // namespace midi_composer::playback

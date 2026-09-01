#pragma once

#include "playback/output_plugin.hpp"
#include "playback/sample_bank.hpp"

#include <array>
#include <vector>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace midi_composer::playback {

/**
 * An output that plays sampled instruments, shaped after the SNES S-DSP.
 *
 * ── What it is, and what it is honestly not ──────────────────────────────────
 *
 * Eight voices, sample playback driven by a pitch *rate* rather than a
 * frequency, per-voice left/right levels, an envelope per voice, voice stealing
 * when a ninth note arrives, and a bank the user supplies. That is the shape.
 *
 * Three things make the real chip sound like itself: the gaussian interpolation
 * kernel, the ADSR rate table, and BRR decoding. None of the three is invented
 * here. Four-point Hermite and a plain seconds-based envelope stand in for the
 * first two, exactly as they do in InternalSynthOutput, and BRR belongs to
 * whatever reads an `.spc` -- by the time audio reaches a Sample it is already
 * decoded. Numbers that look authentic and are not would be worse than a stated
 * approximation, because nobody would ever check them again.
 *
 * ── Why it renders at the host's rate ────────────────────────────────────────
 *
 * The chip runs at 32kHz, and running the DSP there and resampling at the edge
 * would be the authentic arrangement. It is not done yet, because what 32kHz
 * actually changes is the gaussian kernel and the ADSR rate table -- and both
 * are stand-ins. Precision around numbers we do not have is not precision. A
 * sampler resamples inherently, so the rate is a one-line change when the real
 * tables arrive.
 */
class Spc700Output final : public OutputPlugin, public AudioSource {
public:
    Spc700Output();

    [[nodiscard]] std::string_view id() const override { return "spc700"; }
    [[nodiscard]] std::string_view name() const override { return "SPC-700"; }

    [[nodiscard]] AudioSource* audio() override { return this; }

    // ── Configuration ────────────────────────────────────────────────────────

    [[nodiscard]] std::vector<Parameter> parameters() const override;
    [[nodiscard]] ParameterValue get_parameter(std::string_view name) const override;
    base::Result<void> set_parameter(std::string_view name, const ParameterValue& value) override;

    /**
     * How a path becomes instruments.
     *
     * Injected rather than called directly, so this layer keeps knowing nothing
     * about file formats -- the same reason the engine does not know what a
     * track is. The facade supplies the reader; adding one for `.spc` later
     * changes what is passed in here and nothing in this file.
     */
    using BankLoader =
        std::function<base::Result<std::shared_ptr<const SampleBank>>(const std::string&)>;
    void set_bank_loader(BankLoader loader) { m_loader = std::move(loader); }

    /** What the bank parameter currently names, for the status bar. */
    [[nodiscard]] std::string bank_path() const;

    /**
     * Replace the instruments, safely, while sound is playing.
     *
     * The audio thread never touches the old bank after this returns to it: it
     * takes a reference for the length of one block, so the bank being replaced
     * outlives any block still using it and is freed by whoever holds the last
     * reference -- which may well be the audio thread, on its next block, and
     * that is the one cost of doing it this way.
     *
     * Passing null is how a bank is unloaded; the output then makes silence
     * rather than refusing to start.
     */
    void set_bank(std::shared_ptr<const SampleBank> bank);

    /** What is loaded, if anything. Safe to call from any thread. */
    [[nodiscard]] std::shared_ptr<const SampleBank> bank() const;

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

    [[nodiscard]] int sample_rate() const override { return m_sample_rate; }
    void set_sample_rate(int rate) {
        if (rate <= 0) return;
        m_sample_rate = rate;
        resize_echo_line();
    }
    void begin_block(int64_t start_us) override;
    void render(float* interleaved, int frames) override;
    [[nodiscard]] int tail_frames() const override;

    /** Voices sounding right now. For assertions about stealing. */
    [[nodiscard]] int active_voices() const {
        return m_active_voices.load(std::memory_order_relaxed);
    }

    /** Events the queue had no room for. Silence with a reason. */
    [[nodiscard]] uint64_t dropped_events() const {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    static constexpr int kVoices   = 8;
    static constexpr int kChannels = 16;

    // Eight voices summed can clip on their own; the same headroom the internal
    // synth gives itself.
    static constexpr float kVoiceGain = 0.25f;

    struct Event {
        enum class Kind { NoteOn, NoteOff, Controller, ProgramChange, PitchBend, Reset };
        Kind    kind{Kind::Reset};
        uint8_t channel{0};
        int     a{0};
        int     b{0};
        int64_t when_us{0};
    };

    /** Same split as InternalSynthOutput: producers serialise, the reader is
        free. See its EventQueue for why. */
    class EventQueue {
    public:
        bool push(const Event& e);
        bool pop(Event& out);

    private:
        static constexpr size_t kCapacity = 2048;   // power of two
        std::array<Event, kCapacity> m_slots{};
        std::atomic<size_t> m_write{0};
        std::atomic<size_t> m_read{0};
        std::mutex m_producer;
    };

    enum class Stage { Attack, Decay, Sustain, Release };

    struct Voice {
        bool     active{false};
        uint8_t  channel{0};
        uint8_t  pitch{0};
        // Captured when the note starts. Replacing the bank does not retune a
        // note already sounding, and cannot: the sample it is reading may not
        // exist in the new one.
        const Sample* sample{nullptr};
        std::shared_ptr<const SampleBank> holder;   // keeps that sample alive
        double   position{0.0};   // frames into the sample
        double   rate{1.0};       // sample frames per output frame
        float    level{0.0f};     // velocity
        float    envelope{0.0f};
        Stage    stage{Stage::Attack};
        uint64_t started{0};      // for stealing the oldest
    };

    struct Channel {
        float volume{100.0f / 127.0f};
        float pan{0.5f};
        float bend{0.0f};         // semitones
        int   program{0};
    };

    // The longest line the chip allows, so the buffer is sized once and never
    // grown. Allocating on the audio thread to make room for a longer echo
    // would be a dropout exactly when the sound changed.
    static constexpr int kMaxEchoMs = 240;

    /**
     * One frame through the echo, in place.
     *
     * The chip's arrangement rather than a reverb of our own: read the line,
     * run the eight-tap FIR across what comes out, add that to the output, and
     * write the dry signal plus the filtered echo back in. The filter being
     * inside the feedback path is what makes repeats darken instead of just
     * fading, and is most of why this sounds like a room.
     */
    void apply_echo(const EchoSettings& echo, float& left, float& right);

    // Steps of fractional position the interpolation kernel is tabulated at.
    // A power of two so the lookup is a mask rather than a clamp.
    static constexpr size_t kGaussSteps = 256;

    /** The interpolation weights, built once. See the definition for what this
        is and, more importantly, what it is not. */
    static const std::array<std::array<float, 4>, kGaussSteps>& gauss_table();

    void   apply(const Event& e);
    void   reset_voices();
    void   start_note(uint8_t channel, uint8_t pitch, uint8_t velocity);
    void   release_note(uint8_t channel, uint8_t pitch);
    double rate_for(const Sample& sample, uint8_t pitch, float bend) const;
    static float sample_at(const Sample& sample, double position);
    /** Advances one voice's envelope, returning false when it has finished. */
    bool   advance_envelope(Voice& v) const;

    EventQueue m_queue;

    // Audio thread only.
    std::array<Voice, kVoices>     m_voices{};
    std::array<Channel, kChannels> m_channels{};
    Event    m_held;
    bool     m_has_held{false};
    int64_t  m_now_us{0};
    uint64_t m_age{0};
    std::shared_ptr<const SampleBank> m_block_bank;   // this block's bank

    mutable std::mutex m_bank_mutex;
    std::shared_ptr<const SampleBank> m_bank;         // guarded by m_bank_mutex
    std::string m_bank_path;                          // guarded by m_bank_mutex
    BankLoader m_loader;

    // Audio thread only. Sized for the longest delay at the current rate, and
    // used from the start for however much of it the settings ask for.
    std::vector<float> m_echo_line;      // interleaved stereo
    int m_echo_frames{0};                // what the line currently delays by
    int m_echo_write{0};
    std::array<float, 8> m_fir_left{};   // the last eight reads, newest first
    std::array<float, 8> m_fir_right{};

    void resize_echo_line();

    int m_sample_rate{48000};
    std::atomic<int>      m_active_voices{0};
    std::atomic<uint64_t> m_dropped{0};
};

} // namespace midi_composer::playback

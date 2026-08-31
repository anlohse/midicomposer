#pragma once

#include "playback/output_plugin.hpp"

#include <array>
#include <atomic>
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

    [[nodiscard]] int sample_rate() const override { return m_sample_rate; }

    /** The host decides the rate when several outputs share a device (§9a.5).
        32kHz is the chip's, and the default when nobody says otherwise. */
    void set_sample_rate(int rate) { if (rate > 0) m_sample_rate = rate; }
    void begin_block(int64_t start_us) override;
    void render(float* interleaved, int frames) override;
    [[nodiscard]] int tail_frames() const override;

    /** The waveform a channel would use for its next note. */
    [[nodiscard]] int channel_waveform(uint8_t channel) const;

    /** Voices sounding right now. For assertions about stealing. */
    [[nodiscard]] int active_voices() const {
        return m_active_voices.load(std::memory_order_relaxed);
    }

    /** Events the queue had no room for. Silence with a reason beats silence:
        if this is ever non-zero the queue is too small for the traffic. */
    [[nodiscard]] uint64_t dropped_events() const {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    // The chip's rate, which is only the default now: rendering happens at
    // whatever the host asks for, because several outputs share one device.
    // Eight voices is a limit worth keeping: it is the constraint that shaped
    // how music was written for the machine.
    static constexpr int kDefaultSampleRate = 32000;
    static constexpr int kVoices     = 8;
    static constexpr int kChannels   = 16;
    static constexpr int kWaveLength = 256;

    enum class Waveform { Saw, Square, Triangle, Noise };

    struct Event {
        enum class Kind { NoteOn, NoteOff, Controller, ProgramChange, PitchBend, Reset };
        Kind    kind{Kind::Reset};
        uint8_t channel{0};
        int     a{0};
        int     b{0};
        int64_t when_us{0};
    };

    /**
     * How events cross into the audio thread.
     *
     * render() runs on a real-time thread and must not lock, so the consumer
     * side takes nothing. Producers -- the playback thread sending events, a
     * command thread starting or stopping -- serialise among themselves with a
     * mutex, which costs them nothing they cannot afford and keeps the reader
     * free.
     *
     * A fixed capacity means a burst can overflow rather than allocate. That is
     * the right trade on this side of the boundary, and the drop is counted
     * rather than swallowed.
     */
    class EventQueue {
    public:
        bool push(const Event& e);      // any non-audio thread
        bool pop(Event& out);           // audio thread only
        void clear();                   // any non-audio thread

    private:
        static constexpr size_t kCapacity = 2048;   // power of two
        std::array<Event, kCapacity> m_slots{};
        std::atomic<size_t> m_write{0};
        std::atomic<size_t> m_read{0};
        std::mutex m_producer;
    };

    struct Voice {
        bool     active{false};
        uint8_t  channel{0};
        uint8_t  pitch{0};
        // Captured when the note starts. A program change part way through a
        // note changes what comes next, not what is already sounding.
        int      waveform{0};
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
        // Its own timbre. Sixteen channels means sixteen instruments at once,
        // which is what makes one selected output enough for a whole
        // composition: the tracks already carry a channel each.
        int   waveform{0};
        bool  from_program{false};   // false = following the default parameter
    };

    void   apply(const Event& e);
    void   reset_voices();
    void   start_note(uint8_t channel, uint8_t pitch, uint8_t velocity);
    void   release_note(uint8_t channel, uint8_t pitch);
    double rate_for(uint8_t pitch, float bend) const;
    float  sample_at(double phase, int waveform) const;
    [[nodiscard]] int waveform_for(uint8_t channel) const;

    EventQueue m_queue;

    // Touched only by whichever thread is rendering, so they need no
    // synchronisation of their own.
    std::array<Voice, kVoices>     m_voices{};
    std::array<Channel, kChannels> m_channels{};
    Event    m_held;              // popped but not yet due
    bool     m_has_held{false};
    int64_t  m_now_us{0};         // start of the block being rendered
    uint64_t m_age{0};            // monotonic, for voice stealing

    // Every waveform is built once, up front. Switching is then an index rather
    // than a rebuild, which is what keeps a parameter change off the audio
    // thread's back.
    std::array<std::vector<float>, 4> m_waves;
    std::atomic<int> m_waveform{0};

    int m_sample_rate{kDefaultSampleRate};
    std::atomic<int>      m_active_voices{0};
    std::atomic<uint64_t> m_dropped{0};
};

} // namespace midi_composer::playback

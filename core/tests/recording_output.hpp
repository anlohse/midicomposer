#pragma once

#include "playback/output_plugin.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace midi_composer::testing {

/**
 * An OutputPlugin that records what it was asked to play.
 *
 * This is the point of the interface existing, as much as any synth is.
 * MidiService talks to a real port and had no seam, so what playback actually
 * emits could not be asserted -- CC 7, the master fader's scaling and the state
 * restored on a seek all shipped marked "not verified". They are ordinary
 * assertions now.
 *
 * Written to from the playback thread and read from the test's thread, hence
 * the mutex.
 */
class RecordingOutput final : public playback::OutputPlugin {
public:
    struct Event {
        enum class Kind { NoteOn, NoteOff, Controller, ProgramChange, PitchBend };
        Kind    kind;
        uint8_t channel;
        int     a;          // pitch / controller number / program
        int     b;          // velocity / controller value / bend
        int64_t when_us;
    };

    [[nodiscard]] std::string_view id() const override { return "recording"; }
    [[nodiscard]] std::string_view name() const override { return "Recording"; }

    base::Result<void> start() override {
        std::lock_guard lock(m_mutex);
        ++m_starts;
        if (m_start_error) return std::unexpected(*m_start_error);
        return {};
    }

    void stop() override {
        std::lock_guard lock(m_mutex);
        ++m_stops;
    }

    void note_on(uint8_t ch, uint8_t pitch, uint8_t velocity, int64_t when_us) override {
        push({Event::Kind::NoteOn, ch, pitch, velocity, when_us});
    }
    void note_off(uint8_t ch, uint8_t pitch, int64_t when_us) override {
        push({Event::Kind::NoteOff, ch, pitch, 0, when_us});
    }
    void controller(uint8_t ch, uint8_t cc, uint8_t value, int64_t when_us) override {
        push({Event::Kind::Controller, ch, cc, value, when_us});
    }
    void program_change(uint8_t ch, uint8_t program, int64_t when_us) override {
        push({Event::Kind::ProgramChange, ch, program, 0, when_us});
    }
    void pitch_bend(uint8_t ch, int16_t value, int64_t when_us) override {
        push({Event::Kind::PitchBend, ch, value, 0, when_us});
    }

    [[nodiscard]] std::optional<base::Error> failure() const override {
        std::lock_guard lock(m_mutex);
        return m_failure;
    }

    // ── For the test ─────────────────────────────────────────────────────────

    /** Refuse to start, the way a plugin with no device or no samples would. */
    void fail_to_start(base::Error error) {
        std::lock_guard lock(m_mutex);
        m_start_error = std::move(error);
    }

    /** Latch a failure, the way losing a device mid-playback would. */
    void fail_now(base::Error error) {
        std::lock_guard lock(m_mutex);
        m_failure = std::move(error);
    }

    [[nodiscard]] std::vector<Event> events() const {
        std::lock_guard lock(m_mutex);
        return m_events;
    }

    [[nodiscard]] int starts() const {
        std::lock_guard lock(m_mutex);
        return m_starts;
    }

    void clear() {
        std::lock_guard lock(m_mutex);
        m_events.clear();
    }

    /** Every controller value sent for one (channel, controller), in order. */
    [[nodiscard]] std::vector<int> controller_values(uint8_t channel, int controller) const {
        std::lock_guard lock(m_mutex);
        std::vector<int> out;
        for (const auto& e : m_events) {
            if (e.kind == Event::Kind::Controller && e.channel == channel && e.a == controller) {
                out.push_back(e.b);
            }
        }
        return out;
    }

    [[nodiscard]] bool sent_note_on(uint8_t channel, int pitch) const {
        std::lock_guard lock(m_mutex);
        return std::any_of(m_events.begin(), m_events.end(), [&](const Event& e) {
            return e.kind == Event::Kind::NoteOn && e.channel == channel && e.a == pitch;
        });
    }

private:
    void push(Event e) {
        std::lock_guard lock(m_mutex);
        m_events.push_back(e);
    }

    mutable std::mutex m_mutex;
    std::vector<Event> m_events;
    std::optional<base::Error> m_failure;
    std::optional<base::Error> m_start_error;
    int m_starts{0};
    int m_stops{0};
};

} // namespace midi_composer::testing

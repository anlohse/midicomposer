#pragma once

#include "device/midi_service.hpp"
#include "playback/output_plugin.hpp"

#include <atomic>
#include <libremidi/libremidi.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace midi_composer::playback {

/**
 * The default output: a thin layer over the MIDI output the operating system
 * already offers.
 *
 * Thin is the point. Each method here should be little more than packing the
 * bytes and handing them over; anything more elaborate would mean the interface
 * is asking for something the real case cannot give cheaply.
 *
 * Its own libremidi handle, rather than sharing MidiService's: input and output
 * now have different lifetimes -- input is opened once and left alone, output
 * follows the transport -- and one shared object would tie them together for no
 * reason.
 */
class SystemMidiOutput final : public OutputPlugin {
public:
    SystemMidiOutput();
    ~SystemMidiOutput() override;

    [[nodiscard]] std::string_view id() const override { return "system-midi"; }
    [[nodiscard]] std::string_view name() const override { return "System MIDI"; }

    // ── Ports ────────────────────────────────────────────────────────────────
    // Ports are this plugin's own business; nothing above it knows the concept.
    // They will become a declared enum parameter when the configuration schema
    // lands, which is also when the port starts being remembered by name.

    [[nodiscard]] std::vector<device::MidiDeviceInfo> ports() const;
    base::Result<void> open_port(int index);
    void close_port();
    [[nodiscard]] bool is_port_open() const;
    /** Name of the open port, empty when there is none. */
    [[nodiscard]] std::string open_port_name() const;

    /** First available port, so a fresh install makes sound with nothing
        configured. The host has no idea what a reasonable default is. */
    base::Result<void> open_default_port();

    // ── OutputPlugin ─────────────────────────────────────────────────────────

    base::Result<void> start() override;
    void stop() override;

    void note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) override;
    void note_off(uint8_t channel, uint8_t pitch, int64_t when_us) override;
    void controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us) override;
    void program_change(uint8_t channel, uint8_t program, int64_t when_us) override;
    void pitch_bend(uint8_t channel, int16_t value, int64_t when_us) override;

    [[nodiscard]] std::optional<base::Error> failure() const override;

private:
    // `when_us` is ignored throughout: midiOutShortMsg and its equivalents send
    // immediately, and the engine already sends when the event is due. Playing
    // ahead of time would need the streaming API and look-ahead from the engine
    // to match -- see the spec.
    void send(const unsigned char* bytes, size_t size);

    mutable std::mutex m_mutex;                    // guards the port and the handle
    std::unique_ptr<libremidi::midi_out> m_out;
    bool m_open{false};
    std::string m_port_name;

    // Read from the playback thread while the command thread may be writing it,
    // hence atomic rather than the mutex the send path deliberately avoids.
    std::atomic<bool> m_failed{false};
    mutable std::mutex m_failure_mutex;
    base::Error m_failure{};
};

} // namespace midi_composer::playback

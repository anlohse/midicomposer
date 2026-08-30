#pragma once

#include "base/error.hpp"
#include <libremidi/libremidi.hpp>
#include <memory>
#include <string>
#include <vector>

namespace midi_composer::device {

struct MidiDeviceInfo {
    int index;
    std::string name;
};

// MIDI input. Output moved to playback::SystemMidiOutput, which is one
// OutputPlugin among others rather than the only way sound can leave.
class MidiService {
public:
    using MessageCallback = std::function<void(const std::vector<unsigned char>&, double)>;

    MidiService();
    ~MidiService();

    // Device discovery
    std::vector<MidiDeviceInfo> get_input_devices();

    // Port management
    base::Result<void> open_input_port(int index);
    void close_input_port();
    bool is_input_open() const;

    // Callbacks
    void set_input_callback(MessageCallback cb);

private:
    std::unique_ptr<libremidi::midi_in> m_midi_in;

    bool m_input_port_open{false};

    MessageCallback m_input_callback;
};

} // namespace midi_composer::device

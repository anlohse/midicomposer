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

class MidiService {
public:
    using MessageCallback = std::function<void(const std::vector<unsigned char>&, double)>;

    MidiService();
    ~MidiService();

    // Device discovery
    std::vector<MidiDeviceInfo> get_output_devices();
    std::vector<MidiDeviceInfo> get_input_devices();
    
    // Port management
    base::Result<void> open_output_port(int index);
    void close_output_port();
    bool is_output_open() const;

    base::Result<void> open_input_port(int index);
    void close_input_port();
    bool is_input_open() const;

    // Callbacks
    void set_input_callback(MessageCallback cb);

    // Messaging
    void send_message(const std::vector<unsigned char>& message);
    void send_message(const unsigned char* message, size_t size);

private:
    std::unique_ptr<libremidi::midi_out> m_midi_out;
    std::unique_ptr<libremidi::midi_in> m_midi_in;
    
    bool m_output_port_open{false};
    bool m_input_port_open{false};

    MessageCallback m_input_callback;
};

} // namespace midi_composer::device

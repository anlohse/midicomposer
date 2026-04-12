#include "midi_service.hpp"
#include "base/logger.hpp"

namespace midi_composer::device {

MidiService::MidiService() {
    try {
        m_midi_out = std::make_unique<libremidi::midi_out>();
        // libremidi requires a callback even for the initial instance to avoid assertions
        m_midi_in = std::make_unique<libremidi::midi_in>(libremidi::input_configuration{
            .on_message = [](libremidi::message&&) {}
        });
    } catch (const std::exception& e) {
        MC_LOG_ERROR("Failed to initialize libremidi: {}", e.what());
    }
}

MidiService::~MidiService() {
    close_output_port();
    close_input_port();
}

std::vector<MidiDeviceInfo> MidiService::get_output_devices() {
    std::vector<MidiDeviceInfo> devices;
    // We can use the observer without a device instance
    auto observer = libremidi::observer{};
    auto ports = observer.get_output_ports();
    
    for (int i = 0; i < (int)ports.size(); ++i) {
        devices.push_back({i, ports[i].port_name});
    }

    return devices;
}

std::vector<MidiDeviceInfo> MidiService::get_input_devices() {
    std::vector<MidiDeviceInfo> devices;
    auto observer = libremidi::observer{};
    auto ports = observer.get_input_ports();
    
    for (int i = 0; i < (int)ports.size(); ++i) {
        devices.push_back({i, ports[i].port_name});
    }

    return devices;
}

base::Result<void> MidiService::open_output_port(int index) {
    if (!m_midi_out) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure, "MIDI output not initialized"});
    }

    try {
        close_output_port();
        
        auto observer = libremidi::observer{};
        auto ports = observer.get_output_ports();
        
        if (index < 0 || index >= (int)ports.size()) {
            return std::unexpected(base::Error{base::ErrorCode::NotFound, "MIDI output port index out of range"});
        }

        m_midi_out->open_port(ports[index], "MIDI Composer Output");
        m_output_port_open = true;
        
        MC_LOG_INFO("Opened MIDI output port: {}", ports[index].port_name);
        return {};
    } catch (const std::exception& e) {
        m_output_port_open = false;
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure, e.what()});
    }
}

void MidiService::close_output_port() {
    if (m_midi_out && m_output_port_open) {
        m_midi_out->close_port();
        m_output_port_open = false;
        MC_LOG_INFO("Closed MIDI output port");
    }
}

bool MidiService::is_output_open() const {
    return m_output_port_open;
}

base::Result<void> MidiService::open_input_port(int index) {
    if (!m_midi_in) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure, "MIDI input not initialized"});
    }

    try {
        close_input_port();
        
        auto observer = libremidi::observer{};
        auto ports = observer.get_input_ports();
        
        if (index < 0 || index >= (int)ports.size()) {
            return std::unexpected(base::Error{base::ErrorCode::NotFound, "MIDI input port index out of range"});
        }

        // In libremidi 4.x, we pass the configuration when opening the port or creating the instance.
        // Let's recreate the instance with the callback.
        libremidi::input_configuration config;
        config.on_message = [this](libremidi::message&& msg) {
            if (m_input_callback) {
                m_input_callback(msg.bytes, msg.timestamp);
            }
        };
        
        m_midi_in = std::make_unique<libremidi::midi_in>(config);
        m_midi_in->open_port(ports[index], "MIDI Composer Input");
        m_input_port_open = true;
        
        MC_LOG_INFO("Opened MIDI input port: {}", ports[index].port_name);
        return {};
    } catch (const std::exception& e) {
        m_input_port_open = false;
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure, e.what()});
    }
}

void MidiService::close_input_port() {
    if (m_midi_in && m_input_port_open) {
        m_midi_in->close_port();
        m_input_port_open = false;
        MC_LOG_INFO("Closed MIDI input port");
    }
}

bool MidiService::is_input_open() const {
    return m_input_port_open;
}

void MidiService::set_input_callback(MessageCallback cb) {
    m_input_callback = std::move(cb);
}

void MidiService::send_message(const std::vector<unsigned char>& message) {
    if (m_midi_out && m_output_port_open) {
        m_midi_out->send_message(message);
    }
}

void MidiService::send_message(const unsigned char* message, size_t size) {
    if (m_midi_out && m_output_port_open) {
        m_midi_out->send_message(message, size);
    }
}

} // namespace midi_composer::device

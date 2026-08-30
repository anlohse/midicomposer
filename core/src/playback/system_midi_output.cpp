#include "system_midi_output.hpp"

#include "base/logger.hpp"

#include <algorithm>

namespace midi_composer::playback {

SystemMidiOutput::SystemMidiOutput() {
    try {
        m_out = std::make_unique<libremidi::midi_out>();
    } catch (const std::exception& e) {
        MC_LOG_ERROR("Failed to initialize MIDI output: {}", e.what());
    }
}

SystemMidiOutput::~SystemMidiOutput() {
    close_port();
}

std::vector<device::MidiDeviceInfo> SystemMidiOutput::ports() const {
    std::vector<device::MidiDeviceInfo> devices;
    auto observer = libremidi::observer{};
    auto ports = observer.get_output_ports();
    devices.reserve(ports.size());
    for (int i = 0; i < static_cast<int>(ports.size()); ++i) {
        devices.push_back({i, ports[i].port_name});
    }
    return devices;
}

base::Result<void> SystemMidiOutput::open_port(int index) {
    std::lock_guard lock(m_mutex);
    if (!m_out) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           "MIDI output not initialized"});
    }

    try {
        if (m_open) {
            m_out->close_port();
            m_open = false;
            m_port_name.clear();
        }

        auto observer = libremidi::observer{};
        auto ports = observer.get_output_ports();
        if (index < 0 || index >= static_cast<int>(ports.size())) {
            return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                               "MIDI output port index out of range"});
        }

        m_out->open_port(ports[index], "MIDI Composer Output");
        m_open = true;
        m_port_name = ports[index].port_name;
        MC_LOG_INFO("Opened MIDI output port: {}", m_port_name);

        // Opening a port is the operation that fixes whatever went wrong before.
        m_failed.store(false, std::memory_order_release);
        return {};
    } catch (const std::exception& e) {
        m_open = false;
        m_port_name.clear();
        // A failed open can leave the handle holding the device, and winmm then
        // refuses every later attempt -- the output would be dead for the rest
        // of the session with nothing to show for it. Throw the handle away and
        // start from a fresh one.
        try {
            if (m_out) m_out->close_port();
        } catch (...) { /* already unusable; the replacement below is the point */ }
        try {
            m_out = std::make_unique<libremidi::midi_out>();
        } catch (const std::exception& inner) {
            MC_LOG_ERROR("Could not recreate the MIDI output: {}", inner.what());
            m_out.reset();
        }
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure, e.what()});
    }
}

void SystemMidiOutput::close_port() {
    std::lock_guard lock(m_mutex);
    if (m_out && m_open) {
        m_out->close_port();
        m_open = false;
        m_port_name.clear();
        MC_LOG_INFO("Closed MIDI output port");
    }
}

bool SystemMidiOutput::is_port_open() const {
    std::lock_guard lock(m_mutex);
    return m_open;
}

std::string SystemMidiOutput::open_port_name() const {
    std::lock_guard lock(m_mutex);
    return m_port_name;
}

base::Result<void> SystemMidiOutput::open_default_port() {
    const auto available = ports();
    if (available.empty()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "No MIDI output devices found"});
    }
    return open_port(available.front().index);
}

namespace {
constexpr std::string_view kPortParameter = "port";
}

std::vector<Parameter> SystemMidiOutput::parameters() const {
    Parameter port;
    port.name     = std::string(kPortParameter);
    port.label    = "Port";
    port.type     = ParameterType::Enum;
    port.headline = true;   // the one thing worth showing in the status bar
    for (const auto& p : ports()) {
        // Keyed by name, not index: indices shift as devices come and go.
        port.choices.push_back({p.name, p.name});
    }
    return {port};
}

ParameterValue SystemMidiOutput::get_parameter(std::string_view name) const {
    if (name != kPortParameter) return {};
    auto open = open_port_name();
    if (open.empty()) return {};
    return open;
}

base::Result<void> SystemMidiOutput::set_parameter(std::string_view name,
                                                   const ParameterValue& value) {
    if (name != kPortParameter) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Unknown parameter: " + std::string(name)});
    }
    const auto* wanted = std::get_if<std::string>(&value);
    if (!wanted) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "Port must be a name"});
    }
    if (wanted->empty()) {
        close_port();
        return {};
    }

    // Resolved by name every time rather than remembered as an index. Two
    // identical devices share a name and the first one wins, which is worth
    // knowing but is still better than an index that silently becomes a
    // different device.
    for (const auto& p : ports()) {
        if (p.name == *wanted) return open_port(p.index);
    }
    return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                       "MIDI output port '" + *wanted + "' not found"});
}

base::Result<void> SystemMidiOutput::start() {
    // The port is not reopened per transport start. Opening a MIDI device costs
    // tens of milliseconds on some drivers, and stopping and starting playback
    // repeatedly would churn it for nothing. Whether starting closes or opens
    // anything is the plugin's business; the engine only stops sending.
    if (is_port_open()) return {};

    if (auto opened = open_default_port(); !opened) {
        // Said in terms of what is missing, because this is what the user is
        // shown instead of silence.
        return std::unexpected(base::Error{
            opened.error().code,
            "No MIDI output port is open (" + opened.error().message + ")"});
    }
    return {};
}

void SystemMidiOutput::stop() {
    // Deliberately nothing. See start().
}

void SystemMidiOutput::send(const unsigned char* bytes, size_t size) {
    std::lock_guard lock(m_mutex);
    if (!m_out || !m_open) return;
    try {
        m_out->send_message(bytes, size);
    } catch (const std::exception& e) {
        // Latched, not returned: this fails for one reason -- the device went
        // away -- and the engine reads it once per loop rather than per note.
        if (!m_failed.exchange(true, std::memory_order_acq_rel)) {
            std::lock_guard failure_lock(m_failure_mutex);
            m_failure = base::Error{base::ErrorCode::DeviceFailure,
                                    "MIDI output '" + m_port_name + "' failed: " + e.what()};
            MC_LOG_ERROR("MIDI output failed: {}", e.what());
        }
    }
}

std::optional<base::Error> SystemMidiOutput::failure() const {
    if (!m_failed.load(std::memory_order_acquire)) return std::nullopt;
    std::lock_guard lock(m_failure_mutex);
    return m_failure;
}

// ── Events ───────────────────────────────────────────────────────────────────
//
// `when_us` is ignored: these send immediately, which is what the engine wants
// since it already sends when the event is due.

void SystemMidiOutput::note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t) {
    if (channel > 15) channel = 0;
    const unsigned char msg[3] = {
        static_cast<unsigned char>(0x90 | channel),
        static_cast<unsigned char>(pitch & 0x7F),
        static_cast<unsigned char>(velocity & 0x7F),
    };
    send(msg, 3);
}

void SystemMidiOutput::note_off(uint8_t channel, uint8_t pitch, int64_t) {
    if (channel > 15) channel = 0;
    const unsigned char msg[3] = {
        static_cast<unsigned char>(0x80 | channel),
        static_cast<unsigned char>(pitch & 0x7F),
        0,
    };
    send(msg, 3);
}

void SystemMidiOutput::controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t) {
    if (channel > 15) channel = 0;
    const unsigned char msg[3] = {
        static_cast<unsigned char>(0xB0 | channel),
        static_cast<unsigned char>(controller & 0x7F),
        static_cast<unsigned char>(value & 0x7F),
    };
    send(msg, 3);
}

void SystemMidiOutput::program_change(uint8_t channel, uint8_t program, int64_t) {
    if (channel > 15) channel = 0;
    const unsigned char msg[2] = {
        static_cast<unsigned char>(0xC0 | channel),
        static_cast<unsigned char>(program & 0x7F),
    };
    send(msg, 2);
}

void SystemMidiOutput::pitch_bend(uint8_t channel, int16_t value, int64_t) {
    if (channel > 15) channel = 0;
    // On the wire a bend is 14 bits with centre at 8192, split LSB first.
    const int wire = std::clamp(static_cast<int>(value), -8192, 8191) + 8192;
    const unsigned char msg[3] = {
        static_cast<unsigned char>(0xE0 | channel),
        static_cast<unsigned char>(wire & 0x7F),
        static_cast<unsigned char>((wire >> 7) & 0x7F),
    };
    send(msg, 3);
}

} // namespace midi_composer::playback

#include "routing_output.hpp"

#include <algorithm>

namespace midi_composer::playback {

void RoutingOutput::set_default_target(OutputPlugin* target) {
    std::lock_guard lock(m_mutex);
    m_default = target;
}

bool RoutingOutput::set_routes(const std::array<OutputPlugin*, 16>& routes) {
    std::lock_guard lock(m_mutex);
    if (m_routes == routes) return false;
    m_routes = routes;
    return true;
}

OutputPlugin* RoutingOutput::target_for(uint8_t channel) const {
    std::lock_guard lock(m_mutex);
    auto* routed = m_routes[channel & 0x0F];
    return routed ? routed : m_default;
}

std::vector<OutputPlugin*> RoutingOutput::targets() const {
    std::lock_guard lock(m_mutex);
    std::vector<OutputPlugin*> out;
    const auto add = [&out](OutputPlugin* p) {
        if (p && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    add(m_default);
    for (auto* routed : m_routes) add(routed);
    return out;
}

base::Result<void> RoutingOutput::start() {
    // Every reachable target, not just the default: a track routed elsewhere
    // needs its output opened too, and the first failure is what the user is
    // told about.
    for (auto* target : targets()) {
        if (auto started = target->start(); !started) {
            // Named, because with several outputs in play "no port is open"
            // does not say whose, and the one that failed may not be the one
            // the user was listening to.
            return std::unexpected(base::Error{
                started.error().code,
                std::string(target->name()) + ": " + started.error().message});
        }
    }
    return {};
}

void RoutingOutput::stop() {
    for (auto* target : targets()) target->stop();
}

void RoutingOutput::note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) {
    if (auto* t = target_for(channel)) t->note_on(channel, pitch, velocity, when_us);
}

void RoutingOutput::note_off(uint8_t channel, uint8_t pitch, int64_t when_us) {
    if (auto* t = target_for(channel)) t->note_off(channel, pitch, when_us);
}

void RoutingOutput::controller(uint8_t channel, uint8_t cc, uint8_t value, int64_t when_us) {
    if (auto* t = target_for(channel)) t->controller(channel, cc, value, when_us);
}

void RoutingOutput::program_change(uint8_t channel, uint8_t program, int64_t when_us) {
    if (auto* t = target_for(channel)) t->program_change(channel, program, when_us);
}

void RoutingOutput::pitch_bend(uint8_t channel, int16_t value, int64_t when_us) {
    if (auto* t = target_for(channel)) t->pitch_bend(channel, value, when_us);
}

std::optional<base::Error> RoutingOutput::failure() const {
    for (auto* target : targets()) {
        if (auto failed = target->failure()) return failed;
    }
    return std::nullopt;
}

AudioSource* RoutingOutput::audio() {
    AudioSource* found = nullptr;
    for (auto* target : targets()) {
        if (auto* source = target->audio()) {
            if (found) return nullptr;   // more than one: see the header
            found = source;
        }
    }
    return found;
}

} // namespace midi_composer::playback

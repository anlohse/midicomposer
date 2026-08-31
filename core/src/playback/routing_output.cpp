#include "routing_output.hpp"

#include "base/logger.hpp"

#include "playback/clap_instance.hpp"
#include "playback/internal_synth_output.hpp"

#include <algorithm>

namespace midi_composer::playback {

std::string_view RoutingOutput::name() const {
    std::lock_guard lock(m_mutex);
    return m_default ? m_default->name() : std::string_view{"No output"};
}

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
    // Start what can start, rather than refusing everything because one output
    // could not. Which output is the project's is not the same as which ones a
    // composition uses: refusing here left a piece with every track routed
    // elsewhere silent because a MIDI port nothing played through was missing.
    //
    // Only when *nothing* can start is there nothing to play, and that is when
    // the user is told. A failure among several is logged, because a sentence
    // nobody asked for is worse than the music they did.
    std::optional<base::Error> first_failure;
    int started = 0;
    for (auto* target : targets()) {
        if (auto ok = target->start(); ok) {
            ++started;
        } else if (!first_failure) {
            // Named: with several outputs, "no port is open" does not say whose.
            first_failure = base::Error{ok.error().code,
                                        std::string(target->name()) + ": " + ok.error().message};
            MC_LOG_WARN("Output unavailable: {}", first_failure->message);
        }
    }
    if (started == 0 && first_failure) return std::unexpected(*first_failure);
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
    std::vector<AudioSource*> sources;
    for (auto* target : targets()) {
        if (auto* source = target->audio()) sources.push_back(source);
    }
    if (sources.empty()) return nullptr;

    // Rebuilt on every ask rather than cached: the ask happens when routing
    // changed, which is exactly when the set of sources may have.
    m_mixer.set_sources(std::move(sources), m_host_sample_rate);
    return &m_mixer;
}

void RoutingOutput::set_host_sample_rate(int rate) {
    if (rate <= 0) return;
    {
        std::lock_guard lock(m_mutex);
        m_host_sample_rate = rate;
    }
    // Told once, before anything starts. A plugin has to be activated at the
    // rate it will be rendered at, so changing it later would mean stopping
    // and reactivating everything.
    for (auto* target : targets()) {
        if (auto* synth = dynamic_cast<InternalSynthOutput*>(target)) {
            synth->set_sample_rate(rate);
        } else if (auto* clap = dynamic_cast<ClapInstance*>(target)) {
            clap->set_sample_rate(rate);
        }
    }
}

} // namespace midi_composer::playback

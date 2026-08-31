#include "audio_mixer.hpp"

#include <algorithm>

namespace midi_composer::playback {

void AudioMixer::set_sources(std::vector<AudioSource*> sources, int sample_rate) {
    std::lock_guard lock(m_mutex);
    m_sources = std::move(sources);
    m_sample_rate = sample_rate > 0 ? sample_rate : m_sample_rate;
}

size_t AudioMixer::source_count() const {
    std::lock_guard lock(m_mutex);
    return m_sources.size();
}

void AudioMixer::begin_block(int64_t start_us) {
    std::lock_guard lock(m_mutex);
    // Every source is told the same instant, so events land on the same frame
    // whichever output they were routed to.
    for (auto* source : m_sources) source->begin_block(start_us);
}

void AudioMixer::render(float* interleaved, int frames) {
    std::lock_guard lock(m_mutex);
    const size_t samples = static_cast<size_t>(frames) * 2;
    std::fill(interleaved, interleaved + samples, 0.0f);
    if (m_sources.empty()) return;

    if (m_scratch.size() < samples) m_scratch.resize(samples);
    for (auto* source : m_sources) {
        source->render(m_scratch.data(), frames);
        for (size_t i = 0; i < samples; ++i) interleaved[i] += m_scratch[i];
    }

    // Clipped at the sum rather than scaled by the number of sources: dividing
    // would make every instrument quieter the moment another one is added,
    // which is not what adding an instrument should do.
    for (size_t i = 0; i < samples; ++i) {
        interleaved[i] = std::clamp(interleaved[i], -1.0f, 1.0f);
    }
}

int AudioMixer::tail_frames() const {
    std::lock_guard lock(m_mutex);
    int longest = 0;
    for (auto* source : m_sources) longest = std::max(longest, source->tail_frames());
    return longest;
}

} // namespace midi_composer::playback

#pragma once

#include "playback/output_plugin.hpp"

#include <mutex>
#include <vector>

namespace midi_composer::playback {

/**
 * Sums several audio sources into one.
 *
 * Until now an output that made sound was assumed to be the only one, and
 * routing answered that it had no audio at all rather than pick a winner and
 * drop the rest. That was honest and it was also a hole: routing one track to
 * the internal synth and another to a plugin left both silent, which is the
 * first thing anyone tries once plugins work.
 *
 * The mixer is what §9a.6 called the graph, in its smallest useful form: pull
 * each source for the same block, add, hand the sum to the device.
 *
 * No per-source gain here, deliberately. A track's fader reaches its output as
 * CC 7 and is applied inside it, which is the only place it can be applied when
 * several tracks share one multi-timbral instance -- one stereo stream carrying
 * four tracks cannot be attenuated per track from outside. Host-side gain
 * becomes possible when a track owns its instance, and not before.
 */
class AudioMixer final : public AudioSource {
public:
    /** Replace the set of sources. Safe to call while the device is running. */
    void set_sources(std::vector<AudioSource*> sources, int sample_rate);

    [[nodiscard]] size_t source_count() const;

    // ── AudioSource ──────────────────────────────────────────────────────────

    [[nodiscard]] int sample_rate() const override { return m_sample_rate; }
    void begin_block(int64_t start_us) override;
    void render(float* interleaved, int frames) override;
    [[nodiscard]] int tail_frames() const override;

private:
    // Held across begin_block and render, which the device calls in pairs on
    // one thread. A swap between them would mix two different sets.
    mutable std::mutex m_mutex;
    std::vector<AudioSource*> m_sources;
    std::vector<float> m_scratch;
    int m_sample_rate{48000};
};

} // namespace midi_composer::playback

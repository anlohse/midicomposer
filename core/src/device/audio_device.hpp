#pragma once

#include "base/error.hpp"
#include "playback/output_plugin.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace midi_composer::device {

/**
 * The audio device an output that makes its own sound is heard through.
 *
 * It stays open for as long as such an output is selected, rather than being
 * opened when the transport starts: opening a device costs tens of milliseconds
 * and can glitch, and there is nothing to gain from paying that on every press
 * of Play. When nothing is playing the source simply renders silence.
 *
 * Everything below the callback is real time. The callback must not lock,
 * allocate or block, which is why the source it pulls from has to be safe to
 * call that way -- see InternalSynthOutput.
 */
class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    /** Open the default output at the source's rate and start pulling from it. */
    base::Result<void> start(playback::AudioSource& source);
    void stop();

    [[nodiscard]] bool is_running() const { return m_running.load(std::memory_order_acquire); }

    /** Frames handed to the device since it started. The only evidence from
        outside that the callback is actually running. */
    [[nodiscard]] uint64_t frames_rendered() const {
        return m_frames.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string device_name() const;

    /**
     * How far behind real time this device is: the buffer it fills before the
     * hardware plays it.
     *
     * It is the gap between an output the host renders and one that plays
     * elsewhere the instant it is told to, such as a MIDI port, so it is worth
     * being able to see rather than infer.
     */
    [[nodiscard]] double latency_ms() const;

    /** Fill one block. Called from the audio callback and nowhere else; public
        only because the callback is a free function the device driver owns. */
    void pull(float* interleaved, int frames);

private:
    struct Impl;                       // miniaudio stays out of this header
    std::unique_ptr<Impl> m_impl;

    playback::AudioSource* m_source{nullptr};
    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_frames{0};
};

} // namespace midi_composer::device

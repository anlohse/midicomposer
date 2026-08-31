#include "audio_device.hpp"

#include "base/logger.hpp"

// The one translation unit that defines miniaudio. Everything else only
// includes the header for its declarations.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include <chrono>

namespace midi_composer::device {

namespace {

int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct AudioDevice::Impl {
    ma_device device{};
    bool initialised{false};
};

void audio_device_callback(ma_device* device, void* output, const void* /*input*/,
                           ma_uint32 frame_count) {
    auto* self = static_cast<AudioDevice*>(device->pUserData);
    if (self) self->pull(static_cast<float*>(output), static_cast<int>(frame_count));
}

void AudioDevice::pull(float* interleaved, int frames) {
    auto* source = m_source;
    if (!source) return;

    // Real time from here down: no locks, no allocation, no logging.
    //
    // The block is anchored to the wall clock rather than to a frame count, so
    // an event stamped with when it was due lands on the right frame and, more
    // importantly, an event already overdue is applied at once instead of
    // waiting for a counter to catch up to it.
    source->begin_block(steady_us());
    source->render(interleaved, frames);
    m_frames.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
}

AudioDevice::AudioDevice() : m_impl(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
    stop();
}

base::Result<void> AudioDevice::start(playback::AudioSource& source) {
    if (m_running.load(std::memory_order_acquire)) stop();

    const int rate = source.sample_rate();
    if (rate <= 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "The output reported no sample rate"});
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate        = static_cast<ma_uint32>(rate);
    config.dataCallback      = audio_device_callback;
    config.pUserData         = this;

    m_source = &source;
    m_frames.store(0, std::memory_order_relaxed);

    if (ma_device_init(nullptr, &config, &m_impl->device) != MA_SUCCESS) {
        m_source = nullptr;
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           "Could not open an audio output device"});
    }
    m_impl->initialised = true;

    if (ma_device_start(&m_impl->device) != MA_SUCCESS) {
        ma_device_uninit(&m_impl->device);
        m_impl->initialised = false;
        m_source = nullptr;
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           "Could not start the audio output device"});
    }

    m_running.store(true, std::memory_order_release);
    const auto period = m_impl->device.playback.internalPeriodSizeInFrames;
    const auto periods = m_impl->device.playback.internalPeriods;
    const auto actual_rate = m_impl->device.playback.internalSampleRate;
    MC_LOG_INFO("Audio device started: {} at {} Hz (period {} frames x {} = {:.1f} ms)",
                device_name(), rate, period, periods,
                actual_rate ? 1000.0 * period * periods / actual_rate : 0.0);
    return {};
}

void AudioDevice::stop() {
    if (!m_impl->initialised) return;
    // uninit stops the device and joins its thread, so the callback is not
    // running by the time the source pointer is cleared.
    ma_device_uninit(&m_impl->device);
    m_impl->initialised = false;
    m_running.store(false, std::memory_order_release);
    m_source = nullptr;
    MC_LOG_INFO("Audio device stopped");
}

double AudioDevice::latency_ms() const {
    if (!m_impl->initialised) return 0.0;
    const auto frames = m_impl->device.playback.internalPeriodSizeInFrames;
    const auto periods = m_impl->device.playback.internalPeriods;
    const auto rate = m_impl->device.playback.internalSampleRate;
    if (rate == 0) return 0.0;
    return 1000.0 * static_cast<double>(frames) * periods / rate;
}

std::string AudioDevice::device_name() const {
    if (!m_impl->initialised) return {};
    return m_impl->device.playback.name;
}

} // namespace midi_composer::device

#include "internal_synth_output.hpp"

#include <algorithm>
#include <cmath>

namespace midi_composer::playback {

namespace {

constexpr std::string_view kWaveformParameter = "waveform";

// Envelope times. The chip's ADSR is a rate table rather than milliseconds;
// these stand in for it, and are the first thing an SPC-700 plugin would
// replace.
constexpr float kAttackSeconds  = 0.004f;
constexpr float kReleaseSeconds = 0.120f;

// Enough headroom for eight voices at once without clipping the sum.
constexpr float kVoiceGain = 0.11f;

float midi_to_hz(double note) {
    return static_cast<float>(440.0 * std::pow(2.0, (note - 69.0) / 12.0));
}

} // namespace

InternalSynthOutput::InternalSynthOutput() {
    rebuild_wave();
}

// ── Configuration ────────────────────────────────────────────────────────────

std::vector<Parameter> InternalSynthOutput::parameters() const {
    Parameter wave;
    wave.name     = std::string(kWaveformParameter);
    wave.label    = "Waveform";
    wave.type     = ParameterType::Enum;
    wave.headline = true;
    wave.choices  = {
        {"saw", "Saw"}, {"square", "Square"}, {"triangle", "Triangle"}, {"noise", "Noise"},
    };
    return {wave};
}

ParameterValue InternalSynthOutput::get_parameter(std::string_view name) const {
    if (name != kWaveformParameter) return {};
    std::lock_guard lock(m_mutex);
    switch (m_waveform) {
        case Waveform::Saw:      return std::string("saw");
        case Waveform::Square:   return std::string("square");
        case Waveform::Triangle: return std::string("triangle");
        case Waveform::Noise:    return std::string("noise");
    }
    return {};
}

base::Result<void> InternalSynthOutput::set_parameter(std::string_view name,
                                                      const ParameterValue& value) {
    if (name != kWaveformParameter) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Unknown parameter: " + std::string(name)});
    }
    const auto* choice = std::get_if<std::string>(&value);
    if (!choice) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "Waveform must be one of the declared choices"});
    }

    std::lock_guard lock(m_mutex);
    if (*choice == "saw")           m_waveform = Waveform::Saw;
    else if (*choice == "square")   m_waveform = Waveform::Square;
    else if (*choice == "triangle") m_waveform = Waveform::Triangle;
    else if (*choice == "noise")    m_waveform = Waveform::Noise;
    else {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "Unknown waveform: " + *choice});
    }
    rebuild_wave();
    return {};
}

void InternalSynthOutput::rebuild_wave() {
    // Generated rather than shipped: no sample data means no question about
    // where it came from. One cycle, read back at whatever rate a pitch needs.
    m_wave.assign(kWaveLength, 0.0f);
    // A fixed seed, because a rendered file has to be reproducible: the same
    // document must give the same audio, or a golden test means nothing.
    uint32_t noise = 0x1234567u;
    for (int i = 0; i < kWaveLength; ++i) {
        const double t = static_cast<double>(i) / kWaveLength;
        switch (m_waveform) {
            case Waveform::Saw:      m_wave[i] = static_cast<float>(2.0 * t - 1.0); break;
            case Waveform::Square:   m_wave[i] = t < 0.5 ? 1.0f : -1.0f; break;
            case Waveform::Triangle:
                m_wave[i] = static_cast<float>(t < 0.5 ? (4.0 * t - 1.0) : (3.0 - 4.0 * t));
                break;
            case Waveform::Noise:
                noise = noise * 1664525u + 1013904223u;
                m_wave[i] = static_cast<float>((noise >> 8) & 0xFFFF) / 32768.0f - 1.0f;
                break;
        }
    }
}

// ── OutputPlugin ─────────────────────────────────────────────────────────────

base::Result<void> InternalSynthOutput::start() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    for (auto& v : m_voices) v = {};
    return {};
}

void InternalSynthOutput::stop() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    for (auto& v : m_voices) v = {};
}

void InternalSynthOutput::note_on(uint8_t ch, uint8_t pitch, uint8_t velocity, int64_t when_us) {
    std::lock_guard lock(m_mutex);
    m_pending.push_back({Event::Kind::NoteOn, ch, pitch, velocity, when_us});
}

void InternalSynthOutput::note_off(uint8_t ch, uint8_t pitch, int64_t when_us) {
    std::lock_guard lock(m_mutex);
    m_pending.push_back({Event::Kind::NoteOff, ch, pitch, 0, when_us});
}

void InternalSynthOutput::controller(uint8_t ch, uint8_t cc, uint8_t value, int64_t when_us) {
    std::lock_guard lock(m_mutex);
    m_pending.push_back({Event::Kind::Controller, ch, cc, value, when_us});
}

void InternalSynthOutput::program_change(uint8_t, uint8_t, int64_t) {
    // Nothing to change instrument to yet. A bank would be what a program
    // selects from, and there is no bank.
}

void InternalSynthOutput::pitch_bend(uint8_t ch, int16_t value, int64_t when_us) {
    std::lock_guard lock(m_mutex);
    m_pending.push_back({Event::Kind::PitchBend, ch, value, 0, when_us});
}

// ── Voices ───────────────────────────────────────────────────────────────────

double InternalSynthOutput::rate_for(uint8_t pitch, float bend) const {
    // A rate rather than a frequency, the way the DSP addresses a sample: how
    // many wavetable samples to advance per output frame.
    const float hz = midi_to_hz(static_cast<double>(pitch) + bend);
    return static_cast<double>(hz) * kWaveLength / kSampleRate;
}

void InternalSynthOutput::start_note(uint8_t channel, uint8_t pitch, uint8_t velocity) {
    Voice* slot = nullptr;
    for (auto& v : m_voices) {
        if (!v.active) { slot = &v; break; }
    }
    if (!slot) {
        // Eight voices, and a ninth note has to take one. The oldest goes:
        // stealing the newest would cut off what the listener just heard start.
        slot = &m_voices[0];
        for (auto& v : m_voices) {
            if (v.started < slot->started) slot = &v;
        }
    }

    const auto& ch = m_channels[channel & 0x0F];
    *slot = Voice{};
    slot->active    = true;
    slot->channel   = static_cast<uint8_t>(channel & 0x0F);
    slot->pitch     = pitch;
    slot->rate      = rate_for(pitch, ch.bend);
    slot->level     = static_cast<float>(velocity) / 127.0f;
    slot->envelope  = 0.0f;
    slot->releasing = false;
    slot->started   = ++m_age;
}

void InternalSynthOutput::release_note(uint8_t channel, uint8_t pitch) {
    for (auto& v : m_voices) {
        if (v.active && !v.releasing && v.channel == (channel & 0x0F) && v.pitch == pitch) {
            v.releasing = true;
        }
    }
}

void InternalSynthOutput::apply(const Event& e) {
    auto& ch = m_channels[e.channel & 0x0F];
    switch (e.kind) {
        case Event::Kind::NoteOn:
            if (e.b == 0) release_note(e.channel, static_cast<uint8_t>(e.a));
            else          start_note(e.channel, static_cast<uint8_t>(e.a),
                                     static_cast<uint8_t>(e.b));
            break;
        case Event::Kind::NoteOff:
            release_note(e.channel, static_cast<uint8_t>(e.a));
            break;
        case Event::Kind::Controller:
            // The two the mixer sends. Everything else is accepted and ignored
            // rather than refused: an output is not required to implement all
            // of MIDI, only to not fall over.
            if (e.a == 7)       ch.volume = static_cast<float>(e.b) / 127.0f;
            else if (e.a == 10) ch.pan    = static_cast<float>(e.b) / 127.0f;
            break;
        case Event::Kind::PitchBend:
            ch.bend = static_cast<float>(e.a) / 8192.0f * 2.0f;   // ±2 semitones
            for (auto& v : m_voices) {
                if (v.active && v.channel == (e.channel & 0x0F)) {
                    v.rate = rate_for(v.pitch, ch.bend);
                }
            }
            break;
    }
}

float InternalSynthOutput::sample_at(double phase) const {
    // Four-point Hermite. The SNES reads its samples through a gaussian kernel,
    // which is a large part of its character; that table is not reproduced here
    // rather than approximated from memory.
    const int n = static_cast<int>(m_wave.size());
    const double wrapped = phase - std::floor(phase / n) * n;
    const int i1 = static_cast<int>(wrapped);
    const double f = wrapped - i1;
    const float y0 = m_wave[(i1 - 1 + n) % n];
    const float y1 = m_wave[i1 % n];
    const float y2 = m_wave[(i1 + 1) % n];
    const float y3 = m_wave[(i1 + 2) % n];

    const double c0 = y1;
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return static_cast<float>(((c3 * f + c2) * f + c1) * f + c0);
}

// ── AudioSource ──────────────────────────────────────────────────────────────

void InternalSynthOutput::prepare_render(int64_t start_us) {
    std::lock_guard lock(m_mutex);
    m_now_us = start_us;
}

void InternalSynthOutput::render(float* interleaved, int frames) {
    std::lock_guard lock(m_mutex);

    const double us_per_frame = 1'000'000.0 / kSampleRate;
    const float attack  = 1.0f / (kAttackSeconds * kSampleRate);
    const float release = 1.0f / (kReleaseSeconds * kSampleRate);

    // Events carry the instant they were due, so they land on the frame they
    // belong to rather than at the start of the block. Sorted because the
    // engine can deliver a note-off for one note after a note-on for another
    // that starts later in the same slice.
    std::sort(m_pending.begin(), m_pending.end(),
              [](const Event& a, const Event& b) { return a.when_us < b.when_us; });
    size_t next = 0;

    for (int i = 0; i < frames; ++i) {
        const int64_t frame_us = m_now_us + static_cast<int64_t>(i * us_per_frame);
        while (next < m_pending.size() && m_pending[next].when_us <= frame_us) {
            apply(m_pending[next]);
            ++next;
        }

        float left = 0.0f;
        float right = 0.0f;
        for (auto& v : m_voices) {
            if (!v.active) continue;

            if (v.releasing) {
                v.envelope -= release;
                if (v.envelope <= 0.0f) { v.active = false; continue; }
            } else if (v.envelope < 1.0f) {
                v.envelope = std::min(1.0f, v.envelope + attack);
            }

            const float s = sample_at(v.phase) * v.envelope * v.level * kVoiceGain;
            v.phase += v.rate;

            const auto& ch = m_channels[v.channel];
            const float amp = s * ch.volume;
            // Constant-power, so a centred note is not louder than a panned one.
            left  += amp * std::sqrt(1.0f - ch.pan);
            right += amp * std::sqrt(ch.pan);
        }

        interleaved[i * 2]     = std::clamp(left, -1.0f, 1.0f);
        interleaved[i * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }

    // Anything still ahead belongs to a later block; an event that arrived late
    // is applied rather than dropped, which is why this erases only what was
    // consumed.
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<long>(next));
    m_now_us += static_cast<int64_t>(frames * us_per_frame);
}

int InternalSynthOutput::tail_frames() const {
    // Long enough for a release to finish, or a rendered file ends on a click.
    return static_cast<int>(kReleaseSeconds * kSampleRate) + kSampleRate / 10;
}

int InternalSynthOutput::active_voices() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(std::count_if(m_voices.begin(), m_voices.end(),
                                          [](const Voice& v) { return v.active; }));
}

} // namespace midi_composer::playback

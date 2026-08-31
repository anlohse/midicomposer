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

const char* const kWaveformNames[] = {"saw", "square", "triangle", "noise"};

// Which waveform a General MIDI program lands on, by family of eight. Four
// waveforms cannot be 128 instruments, but they can be told apart, and the
// grouping is what makes a piano track and a brass track sound like different
// things rather than the same thing twice.
constexpr int kSaw = 0, kSquare = 1, kTriangle = 2, kNoise = 3;
constexpr int kFamilyWaveform[16] = {
    kTriangle,  //   0 Piano
    kTriangle,  //   8 Chromatic percussion
    kSquare,    //  16 Organ
    kSaw,       //  24 Guitar
    kSquare,    //  32 Bass
    kSaw,       //  40 Strings
    kSaw,       //  48 Ensemble
    kSaw,       //  56 Brass
    kSquare,    //  64 Reed
    kTriangle,  //  72 Pipe
    kSaw,       //  80 Synth lead
    kTriangle,  //  88 Synth pad
    kNoise,     //  96 Synth effects
    kSquare,    // 104 Ethnic
    kNoise,     // 112 Percussive
    kNoise,     // 120 Sound effects
};

// General MIDI reserves channel 10 for percussion, whatever program it carries.
// The metronome runs there too, so its click stops being a pitched note.
constexpr uint8_t kPercussionChannel = 9;

float midi_to_hz(double note) {
    return static_cast<float>(440.0 * std::pow(2.0, (note - 69.0) / 12.0));
}

} // namespace

// ── The queue ────────────────────────────────────────────────────────────────

bool InternalSynthOutput::EventQueue::push(const Event& e) {
    std::lock_guard lock(m_producer);
    const size_t write = m_write.load(std::memory_order_relaxed);
    const size_t next = (write + 1) % kCapacity;
    if (next == m_read.load(std::memory_order_acquire)) return false;   // full
    m_slots[write] = e;
    m_write.store(next, std::memory_order_release);
    return true;
}

bool InternalSynthOutput::EventQueue::pop(Event& out) {
    const size_t read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire)) return false;  // empty
    out = m_slots[read];
    m_read.store((read + 1) % kCapacity, std::memory_order_release);
    return true;
}

void InternalSynthOutput::EventQueue::clear() {
    std::lock_guard lock(m_producer);
    // Moving the read index is the consumer's job, so this drops what is
    // pending by making the queue look empty from the writer's side only when
    // the reader gets there. Pushing a Reset is the honest way to silence the
    // voices; this is only for a source that is not being rendered at all.
    m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release);
}

// ── Construction ─────────────────────────────────────────────────────────────

InternalSynthOutput::InternalSynthOutput() {
    // All four built once. Switching waveform is then an index, not a rebuild,
    // which is what keeps a parameter change from touching memory the audio
    // thread is reading.
    for (int w = 0; w < 4; ++w) {
        auto& wave = m_waves[static_cast<size_t>(w)];
        wave.assign(kWaveLength, 0.0f);
        // A fixed seed: a rendered file has to be reproducible, or a test that
        // compares two renders means nothing.
        uint32_t noise = 0x1234567u;
        for (int i = 0; i < kWaveLength; ++i) {
            const double t = static_cast<double>(i) / kWaveLength;
            switch (static_cast<Waveform>(w)) {
                case Waveform::Saw:      wave[i] = static_cast<float>(2.0 * t - 1.0); break;
                case Waveform::Square:   wave[i] = t < 0.5 ? 1.0f : -1.0f; break;
                case Waveform::Triangle:
                    wave[i] = static_cast<float>(t < 0.5 ? (4.0 * t - 1.0) : (3.0 - 4.0 * t));
                    break;
                case Waveform::Noise:
                    noise = noise * 1664525u + 1013904223u;
                    wave[i] = static_cast<float>((noise >> 8) & 0xFFFF) / 32768.0f - 1.0f;
                    break;
            }
        }
    }
}

// ── Configuration ────────────────────────────────────────────────────────────

std::vector<Parameter> InternalSynthOutput::parameters() const {
    Parameter wave;
    wave.name     = std::string(kWaveformParameter);
    wave.label    = "Default waveform";
    wave.type     = ParameterType::Enum;
    wave.headline = true;
    // The default only. A track that selects an instrument gets the waveform
    // its General MIDI family maps to, so four tracks are four timbres without
    // touching this.
    wave.choices  = {
        {"saw", "Saw"}, {"square", "Square"}, {"triangle", "Triangle"}, {"noise", "Noise"},
    };
    return {wave};
}

ParameterValue InternalSynthOutput::get_parameter(std::string_view name) const {
    if (name != kWaveformParameter) return {};
    return std::string(kWaveformNames[m_waveform.load(std::memory_order_relaxed)]);
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
    for (int i = 0; i < 4; ++i) {
        if (*choice == kWaveformNames[i]) {
            // A single atomic store. The audio thread picks it up on its next
            // frame and reads a table that was already there.
            m_waveform.store(i, std::memory_order_relaxed);
            return {};
        }
    }
    return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                       "Unknown waveform: " + *choice});
}

// ── OutputPlugin ─────────────────────────────────────────────────────────────

base::Result<void> InternalSynthOutput::start() {
    // Through the queue rather than reaching into the voices: a device may
    // already be pulling, and this is called from a command thread.
    m_queue.push(Event{Event::Kind::Reset, 0, 0, 0, 0});
    return {};
}

void InternalSynthOutput::stop() {
    m_queue.push(Event{Event::Kind::Reset, 0, 0, 0, 0});
}

void InternalSynthOutput::note_on(uint8_t ch, uint8_t pitch, uint8_t velocity, int64_t when_us) {
    if (!m_queue.push({Event::Kind::NoteOn, ch, pitch, velocity, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void InternalSynthOutput::note_off(uint8_t ch, uint8_t pitch, int64_t when_us) {
    if (!m_queue.push({Event::Kind::NoteOff, ch, pitch, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void InternalSynthOutput::controller(uint8_t ch, uint8_t cc, uint8_t value, int64_t when_us) {
    if (!m_queue.push({Event::Kind::Controller, ch, cc, value, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void InternalSynthOutput::program_change(uint8_t ch, uint8_t program, int64_t when_us) {
    if (!m_queue.push({Event::Kind::ProgramChange, ch, program, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void InternalSynthOutput::pitch_bend(uint8_t ch, int16_t value, int64_t when_us) {
    if (!m_queue.push({Event::Kind::PitchBend, ch, value, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Voices ───────────────────────────────────────────────────────────────────

double InternalSynthOutput::rate_for(uint8_t pitch, float bend) const {
    // A rate rather than a frequency, the way the DSP addresses a sample: how
    // many wavetable samples to advance per output frame.
    const float hz = midi_to_hz(static_cast<double>(pitch) + bend);
    return static_cast<double>(hz) * kWaveLength / m_sample_rate;
}

void InternalSynthOutput::reset_voices() {
    for (auto& v : m_voices) v = {};
    // Channels go back to defaults too. Everything that was set on them --
    // programs, the mixer, bends -- is re-sent as the transport starts, so
    // keeping the old values would only let a stale one survive.
    for (auto& c : m_channels) c = {};
    m_active_voices.store(0, std::memory_order_relaxed);
}

int InternalSynthOutput::waveform_for(uint8_t channel) const {
    const auto& ch = m_channels[channel & 0x0F];
    if ((channel & 0x0F) == kPercussionChannel) return kNoise;
    // A channel nobody chose an instrument for follows the parameter, so the
    // setting still means something on a document with no program changes.
    if (!ch.from_program) return m_waveform.load(std::memory_order_relaxed);
    return ch.waveform;
}

int InternalSynthOutput::channel_waveform(uint8_t channel) const {
    return waveform_for(channel);
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
    slot->waveform  = waveform_for(channel);
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
        case Event::Kind::Reset:
            reset_voices();
            break;
        case Event::Kind::NoteOn:
            if (e.b == 0) release_note(e.channel, static_cast<uint8_t>(e.a));
            else          start_note(e.channel, static_cast<uint8_t>(e.a),
                                     static_cast<uint8_t>(e.b));
            break;
        case Event::Kind::NoteOff:
            release_note(e.channel, static_cast<uint8_t>(e.a));
            break;
        case Event::Kind::ProgramChange:
            ch.waveform = kFamilyWaveform[(e.a & 0x7F) / 8];
            ch.from_program = true;
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

float InternalSynthOutput::sample_at(double phase, int waveform) const {
    // Four-point Hermite. The SNES reads its samples through a gaussian kernel,
    // which is a large part of its character; that table is not reproduced here
    // rather than approximated from memory.
    const auto& wave = m_waves[static_cast<size_t>(waveform)];
    const int n = static_cast<int>(wave.size());
    const double wrapped = phase - std::floor(phase / n) * n;
    const int i1 = static_cast<int>(wrapped);
    const double f = wrapped - i1;
    const float y0 = wave[(i1 - 1 + n) % n];
    const float y1 = wave[i1 % n];
    const float y2 = wave[(i1 + 1) % n];
    const float y3 = wave[(i1 + 2) % n];

    const double c0 = y1;
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return static_cast<float>(((c3 * f + c2) * f + c1) * f + c0);
}

// ── AudioSource ──────────────────────────────────────────────────────────────

void InternalSynthOutput::begin_block(int64_t start_us) {
    m_now_us = start_us;
}

void InternalSynthOutput::render(float* interleaved, int frames) {
    // Real time from here down. No locks, no allocation: the queue is popped
    // without taking anything, the wavetables were built at construction, and
    // the voices belong to this thread.
    const double us_per_frame = 1'000'000.0 / m_sample_rate;
    const float attack  = 1.0f / (kAttackSeconds * m_sample_rate);
    const float release = 1.0f / (kReleaseSeconds * m_sample_rate);

    for (int i = 0; i < frames; ++i) {
        const int64_t frame_us = m_now_us + static_cast<int64_t>(i * us_per_frame);

        // An event popped but not yet due is held rather than pushed back: the
        // queue only moves one way, and holding one is enough because events
        // arrive in the order they were scheduled.
        while (true) {
            if (!m_has_held) {
                if (!m_queue.pop(m_held)) break;
                m_has_held = true;
            }
            // A Reset is not scheduled against the music, so it applies at once
            // however the clocks happen to line up.
            if (m_held.kind != Event::Kind::Reset && m_held.when_us > frame_us) break;
            apply(m_held);
            m_has_held = false;
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

            const float s = sample_at(v.phase, v.waveform) * v.envelope * v.level * kVoiceGain;
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

    int active = 0;
    for (const auto& v : m_voices) if (v.active) ++active;
    m_active_voices.store(active, std::memory_order_relaxed);
}

int InternalSynthOutput::tail_frames() const {
    // Long enough for a release to finish, or a rendered file ends on a click.
    return static_cast<int>(kReleaseSeconds * m_sample_rate) + m_sample_rate / 10;
}

} // namespace midi_composer::playback

#include "spc700_output.hpp"

#include <algorithm>
#include <cmath>

namespace midi_composer::playback {

// ── EventQueue ───────────────────────────────────────────────────────────────

bool Spc700Output::EventQueue::push(const Event& e) {
    std::lock_guard lock(m_producer);
    const size_t write = m_write.load(std::memory_order_relaxed);
    const size_t next = (write + 1) & (kCapacity - 1);
    if (next == m_read.load(std::memory_order_acquire)) return false;   // full
    m_slots[write] = e;
    m_write.store(next, std::memory_order_release);
    return true;
}

bool Spc700Output::EventQueue::pop(Event& out) {
    const size_t read = m_read.load(std::memory_order_relaxed);
    if (read == m_write.load(std::memory_order_acquire)) return false;  // empty
    out = m_slots[read];
    m_read.store((read + 1) & (kCapacity - 1), std::memory_order_release);
    return true;
}

// ── Construction and the bank ────────────────────────────────────────────────

Spc700Output::Spc700Output() = default;

void Spc700Output::set_bank(std::shared_ptr<const SampleBank> bank) {
    {
        std::lock_guard lock(m_bank_mutex);
        m_bank = std::move(bank);
    }
    // Notes already sounding keep reading the bank they started on -- each
    // voice holds a reference for exactly that reason -- so a swap is heard as
    // the next note, not as a click in the middle of this one.
    m_queue.push(Event{Event::Kind::Reset, 0, 0, 0, 0});
}

std::shared_ptr<const SampleBank> Spc700Output::bank() const {
    std::lock_guard lock(m_bank_mutex);
    return m_bank;
}

std::string Spc700Output::bank_path() const {
    std::lock_guard lock(m_bank_mutex);
    return m_bank_path;
}

// ── Configuration ────────────────────────────────────────────────────────────

std::vector<Parameter> Spc700Output::parameters() const {
    Parameter bank;
    bank.name     = "bank";
    bank.label    = "Sample bank";
    bank.type     = ParameterType::File;
    bank.headline = true;      // what the status bar shows beside the name
    bank.filter   = "*.sf2";
    return {bank};
}

ParameterValue Spc700Output::get_parameter(std::string_view name) const {
    if (name != "bank") return {};
    std::lock_guard lock(m_bank_mutex);
    if (m_bank_path.empty()) return {};   // monostate: nothing loaded
    return m_bank_path;
}

base::Result<void> Spc700Output::set_parameter(std::string_view name,
                                               const ParameterValue& value) {
    if (name != "bank") {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Unknown parameter: " + std::string(name)});
    }
    const auto* path = std::get_if<std::string>(&value);
    if (!path) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "A sample bank is a path"});
    }

    if (path->empty()) {
        // Clearing is a legitimate choice, and the only way back to silence
        // without restarting.
        set_bank(nullptr);
        std::lock_guard lock(m_bank_mutex);
        m_bank_path.clear();
        return {};
    }
    if (!m_loader) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState,
                                           "No sample bank reader is available"});
    }

    // Loaded before anything is replaced, so a file that will not open leaves
    // the instruments that were already playing exactly as they were.
    auto loaded = m_loader(*path);
    if (!loaded) return std::unexpected(loaded.error());

    set_bank(*loaded);
    std::lock_guard lock(m_bank_mutex);
    m_bank_path = *path;
    return {};
}

// ── OutputPlugin ─────────────────────────────────────────────────────────────

base::Result<void> Spc700Output::start() {
    // No bank is not a failure to start. An output that refuses because nothing
    // is loaded would take a whole composition down over one silent track, and
    // §8 has this as playing rather than falling over.
    m_queue.push(Event{Event::Kind::Reset, 0, 0, 0, 0});
    return {};
}

void Spc700Output::stop() {
    m_queue.push(Event{Event::Kind::Reset, 0, 0, 0, 0});
}

void Spc700Output::note_on(uint8_t ch, uint8_t pitch, uint8_t velocity, int64_t when_us) {
    if (!m_queue.push({Event::Kind::NoteOn, ch, pitch, velocity, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Spc700Output::note_off(uint8_t ch, uint8_t pitch, int64_t when_us) {
    if (!m_queue.push({Event::Kind::NoteOff, ch, pitch, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Spc700Output::controller(uint8_t ch, uint8_t cc, uint8_t value, int64_t when_us) {
    if (!m_queue.push({Event::Kind::Controller, ch, cc, value, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Spc700Output::program_change(uint8_t ch, uint8_t program, int64_t when_us) {
    if (!m_queue.push({Event::Kind::ProgramChange, ch, program, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Spc700Output::pitch_bend(uint8_t ch, int16_t value, int64_t when_us) {
    if (!m_queue.push({Event::Kind::PitchBend, ch, value, 0, when_us})) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Voices ───────────────────────────────────────────────────────────────────

double Spc700Output::rate_for(const Sample& sample, uint8_t pitch, float bend) const {
    // A rate, the way the DSP addresses a sample: how many sample frames to
    // advance per output frame. Two things are folded in -- the interval from
    // the pitch the sample was recorded at, and the ratio between the sample's
    // own rate and the one being rendered at.
    const double semitones = static_cast<double>(pitch) + bend -
                             static_cast<double>(sample.root_key) +
                             sample.fine_tune_cents / 100.0;
    const double ratio = std::pow(2.0, semitones / 12.0);
    return ratio * static_cast<double>(sample.source_rate) / m_sample_rate;
}

void Spc700Output::reset_voices() {
    for (auto& v : m_voices) v = {};
    for (auto& c : m_channels) c = {};
    m_active_voices.store(0, std::memory_order_relaxed);
}

void Spc700Output::start_note(uint8_t channel, uint8_t pitch, uint8_t velocity) {
    if (!m_block_bank) return;    // nothing loaded: the note is simply silent
    const auto& ch = m_channels[channel & 0x0F];
    const Sample* sample = m_block_bank->for_program(ch.program);
    if (!sample || sample->data.empty()) return;

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

    *slot = Voice{};
    slot->active   = true;
    slot->channel  = static_cast<uint8_t>(channel & 0x0F);
    slot->pitch    = pitch;
    slot->sample   = sample;
    // The bank, not just the sample: this is what stops a swap from pulling the
    // audio out from under a note that is still sounding.
    slot->holder   = m_block_bank;
    slot->position = 0.0;
    slot->rate     = rate_for(*sample, pitch, ch.bend);
    slot->level    = static_cast<float>(velocity) / 127.0f;
    slot->envelope = 0.0f;
    slot->stage    = Stage::Attack;
    slot->started  = ++m_age;
}

void Spc700Output::release_note(uint8_t channel, uint8_t pitch) {
    for (auto& v : m_voices) {
        if (v.active && v.stage != Stage::Release && v.channel == (channel & 0x0F) &&
            v.pitch == pitch) {
            v.stage = Stage::Release;
        }
    }
}

void Spc700Output::apply(const Event& e) {
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
            // Takes effect on the next note. A program change part way through
            // one changes what comes next, not what is already sounding.
            ch.program = e.a & 0x7F;
            break;
        case Event::Kind::Controller:
            if (e.a == 7)       ch.volume = static_cast<float>(e.b) / 127.0f;
            else if (e.a == 10) ch.pan    = static_cast<float>(e.b) / 127.0f;
            break;
        case Event::Kind::PitchBend:
            ch.bend = static_cast<float>(e.a) / 8192.0f * 2.0f;   // ±2 semitones
            for (auto& v : m_voices) {
                if (v.active && v.sample && v.channel == (e.channel & 0x0F)) {
                    v.rate = rate_for(*v.sample, v.pitch, ch.bend);
                }
            }
            break;
    }
}

float Spc700Output::sample_at(const Sample& sample, double position) {
    // Four-point Hermite, the same stand-in the internal synth uses. The chip
    // reads through a gaussian kernel, which is a large part of its character;
    // that table is not reproduced here rather than guessed at.
    const auto& data = sample.data;
    const int n = static_cast<int>(data.size());
    if (n == 0) return 0.0f;

    const int i1 = static_cast<int>(position);
    const double f = position - i1;

    // Clamped at the ends rather than wrapped: a sample that is not looping has
    // no frame after its last one, and wrapping would fold the attack onto the
    // decay.
    const auto at = [&data, n](int i) {
        return data[static_cast<size_t>(std::clamp(i, 0, n - 1))];
    };
    const float y0 = at(i1 - 1);
    const float y1 = at(i1);
    const float y2 = at(i1 + 1);
    const float y3 = at(i1 + 2);

    const double c0 = y1;
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return static_cast<float>(((c3 * f + c2) * f + c1) * f + c0);
}

bool Spc700Output::advance_envelope(Voice& v) const {
    const auto& s = *v.sample;
    const auto per_frame = [this](float seconds) {
        return seconds <= 0.0f ? 1.0f : 1.0f / (seconds * static_cast<float>(m_sample_rate));
    };

    switch (v.stage) {
        case Stage::Attack:
            v.envelope += per_frame(s.attack);
            if (v.envelope >= 1.0f) { v.envelope = 1.0f; v.stage = Stage::Decay; }
            break;
        case Stage::Decay:
            if (s.decay <= 0.0f || v.envelope <= s.sustain) {
                v.envelope = s.sustain;
                v.stage = Stage::Sustain;
            } else {
                v.envelope -= (1.0f - s.sustain) * per_frame(s.decay);
                if (v.envelope <= s.sustain) { v.envelope = s.sustain; v.stage = Stage::Sustain; }
            }
            break;
        case Stage::Sustain:
            // A sustain of zero is an instrument that dies on its own -- a
            // plucked string, most of a percussion kit -- and holding the key
            // does not bring it back.
            if (v.envelope <= 0.0f) return false;
            break;
        case Stage::Release:
            v.envelope -= per_frame(s.release);
            if (v.envelope <= 0.0f) return false;
            break;
    }
    return true;
}

// ── AudioSource ──────────────────────────────────────────────────────────────

void Spc700Output::begin_block(int64_t start_us) {
    m_now_us = start_us;
    // The one place the bank is picked up. Taking it here rather than per frame
    // means a block plays one bank throughout, and the reference keeps it alive
    // for the whole of render() no matter what another thread does meanwhile.
    std::lock_guard lock(m_bank_mutex);
    m_block_bank = m_bank;
}

void Spc700Output::render(float* interleaved, int frames) {
    const double us_per_frame = 1'000'000.0 / m_sample_rate;

    for (int i = 0; i < frames; ++i) {
        const int64_t frame_us = m_now_us + static_cast<int64_t>(i * us_per_frame);

        while (true) {
            if (!m_has_held) {
                if (!m_queue.pop(m_held)) break;
                m_has_held = true;
            }
            if (m_held.kind != Event::Kind::Reset && m_held.when_us > frame_us) break;
            apply(m_held);
            m_has_held = false;
        }

        float left = 0.0f;
        float right = 0.0f;
        for (auto& v : m_voices) {
            if (!v.active || !v.sample) continue;

            if (!advance_envelope(v)) {
                v = {};   // releases this voice's hold on its bank
                continue;
            }

            const auto& s = *v.sample;
            const float raw = sample_at(s, v.position) * v.envelope * v.level * kVoiceGain;
            v.position += v.rate;

            if (s.loop_start >= 0 && s.loop_end > s.loop_start) {
                if (v.position >= s.loop_end) {
                    const double span = static_cast<double>(s.loop_end - s.loop_start);
                    v.position = s.loop_start + std::fmod(v.position - s.loop_end, span);
                }
            } else if (v.position >= static_cast<double>(s.data.size())) {
                // Ran off the end of a one-shot. Not a release: there is simply
                // nothing left to read.
                v = {};
                continue;
            }

            const auto& ch = m_channels[v.channel];
            const float amp = raw * ch.volume;
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

int Spc700Output::tail_frames() const {
    // Long enough for the slowest release a bank declares to finish, or a
    // rendered file ends on a click.
    float longest = 0.1f;
    if (const auto bank = this->bank()) {
        for (const auto& s : bank->samples) longest = std::max(longest, s.release);
    }
    return static_cast<int>(longest * m_sample_rate) + m_sample_rate / 10;
}

} // namespace midi_composer::playback

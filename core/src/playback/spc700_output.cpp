#include "spc700_output.hpp"

#include "base/logger.hpp"

#include "playback/gaussian_table.hpp"

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

Spc700Output::Spc700Output() { resize_echo_line(); }

void Spc700Output::resize_echo_line() {
    // Room for the longest delay the chip allows, once. The settings then use
    // however much of it they ask for, so changing a rip's echo never allocates.
    const size_t frames = static_cast<size_t>(kMaxEchoMs) * m_sample_rate / 1000 + 1;
    m_echo_line.assign(frames * 2, 0.0f);
    m_echo_frames = 0;
    m_echo_write = 0;
    m_fir_left.fill(0.0f);
    m_fir_right.fill(0.0f);
}

void Spc700Output::apply_echo(const EchoSettings& echo, float& left, float& right,
                              float send_left, float send_right) {
    const int frames = std::min(echo.delay_ms * m_sample_rate / 1000,
                                static_cast<int>(m_echo_line.size() / 2) - 1);
    if (frames <= 0) return;

    if (frames != m_echo_frames) {
        // The line length changed under us -- a different rip, usually. Start
        // it clear rather than reading whatever the old length left behind,
        // which would arrive as a burst of the previous piece.
        m_echo_frames = frames;
        m_echo_write = 0;
        std::fill(m_echo_line.begin(), m_echo_line.end(), 0.0f);
        m_fir_left.fill(0.0f);
        m_fir_right.fill(0.0f);
    }

    // Read where we are about to write: the whole buffer is the delay.
    const size_t at = static_cast<size_t>(m_echo_write) * 2;
    const float delayed_left = m_echo_line[at];
    const float delayed_right = m_echo_line[at + 1];

    // Newest first, so tap 0 weights the most recent sample out of the line.
    for (int i = 7; i > 0; --i) {
        m_fir_left[static_cast<size_t>(i)] = m_fir_left[static_cast<size_t>(i - 1)];
        m_fir_right[static_cast<size_t>(i)] = m_fir_right[static_cast<size_t>(i - 1)];
    }
    m_fir_left[0] = delayed_left;
    m_fir_right[0] = delayed_right;

    float filtered_left = 0.0f;
    float filtered_right = 0.0f;
    for (size_t i = 0; i < 8; ++i) {
        filtered_left += m_fir_left[i] * echo.fir[i];
        filtered_right += m_fir_right[i] * echo.fir[i];
    }

    // Written back before the echo is added to the output, so a voice hears
    // itself once per pass rather than twice.
    m_echo_line[at] = std::clamp(send_left + filtered_left * echo.feedback, -4.0f, 4.0f);
    m_echo_line[at + 1] = std::clamp(send_right + filtered_right * echo.feedback, -4.0f, 4.0f);
    // Clamped rather than trusted: the filter can have gain above one, and with
    // feedback near one that compounds every pass. The chip wraps here; a
    // rendered file would rather be loud than be a square wave.

    left += filtered_left * echo.volume_left;
    right += filtered_right * echo.volume_right;

    m_echo_write = (m_echo_write + 1) % m_echo_frames;
}

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
    bank.filter   = "*.sf2;*.spc";
    return {bank};
}

ParameterValue Spc700Output::get_parameter(std::string_view name) const {
    if (name != "bank") return {};
    std::lock_guard lock(m_bank_mutex);
    if (m_bank_path.empty()) return {};   // monostate: nothing loaded
    return m_bank_path;
}

std::vector<Spc700Output::ProgramInfo> Spc700Output::programs() const {
    const auto loaded = bank();
    if (!loaded) return {};      // nothing loaded: leave the list alone
    std::vector<ProgramInfo> out;
    for (int program = 0; program < 128; ++program) {
        // Only what the bank actually filled. A rip has two dozen instruments,
        // and offering a hundred empty slots would bury them.
        if (!loaded->has_program(program)) continue;
        out.push_back({program, loaded->programs[static_cast<size_t>(program)].name});
    }
    return out;
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

double Spc700Output::rate_for(const Sample& sample, const Zone& zone, uint8_t pitch,
                              float bend) const {
    // A rate, the way the DSP addresses a sample: how many sample frames to
    // advance per output frame. Two things are folded in -- the interval from
    // the pitch the sample was recorded at, and the ratio between the sample's
    // own rate and the one being rendered at.
    const double semitones = static_cast<double>(pitch) + bend -
                             static_cast<double>(zone.root_key) +
                             zone.fine_tune_cents / 100.0;
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

    // The zone decides, not the program: a multi-sampled instrument answers
    // differently at each end of the keyboard, which is the point of zones.
    // Several may answer at once -- a preset that layers two instruments, or
    // one instrument covering a key twice -- and all of them sound. On eight
    // voices that is expensive, and it is meant to be: the alternative is
    // playing the first and calling the result the instrument.
    const Zone* matched[kVoices] = {};
    const int count = m_block_bank->zones_for(ch.program, pitch, velocity, matched, kVoices);
    // A note that matches nothing is the one failure a musician cannot see:
    // the score, the mixer and the instrument list all look right and the note
    // makes no sound. Rare by construction, so saying so costs nothing.
    if (count == 0) {
        MC_LOG_WARN("Nothing in program {} answers key {} at velocity {}",
                    ch.program + 1, pitch, velocity);
    } else if (m_trace_notes) {
        MC_LOG_INFO("note ch={} program={} key={} velocity={} voices={}",
                    channel & 0x0F, ch.program + 1, pitch, velocity, count);
    }

    for (int i = 0; i < count; ++i) {
        const Zone* zone = matched[i];
        const Sample* sample = m_block_bank->sample_of(*zone);
        if (!sample || sample->data.empty()) continue;

        Voice* slot = nullptr;
        for (auto& v : m_voices) {
            if (!v.active) { slot = &v; break; }
        }
        if (!slot) {
            // Eight voices, and a ninth note has to take one. The oldest goes:
            // stealing the newest would cut off what the listener just heard
            // start -- which also protects the layers started just above, since
            // they are the newest things in the box.
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
        slot->zone     = zone;
        // The bank, not just the sample: this is what stops a swap from pulling
        // the audio out from under a note that is still sounding.
        slot->holder   = m_block_bank;
        slot->position = 0.0;
        slot->rate     = rate_for(*sample, *zone, pitch, ch.bend);
        slot->level    = static_cast<float>(velocity) / 127.0f * zone->gain;
        slot->envelope = 0.0f;
        slot->stage    = Stage::Attack;
        slot->started  = ++m_age;
    }
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
                if (v.active && v.sample && v.zone && v.channel == (e.channel & 0x0F)) {
                    v.rate = rate_for(*v.sample, *v.zone, v.pitch, ch.bend);
                }
            }
            break;
    }
}

float Spc700Output::sample_at(const Sample& sample, double position) {
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
    // The chip's own kernel, indexed the way the chip indexes it. See
    // gaussian_table.hpp for where the numbers come from and what was checked.
    const size_t p = std::min<size_t>(255, static_cast<size_t>(f * 256.0));
    const float weighted = at(i1 - 1) * kGaussTable[255 - p] +
                           at(i1)     * kGaussTable[511 - p] +
                           at(i1 + 1) * kGaussTable[256 + p] +
                           at(i1 + 2) * kGaussTable[p];
    return weighted / static_cast<float>(kGaussUnity);
}

bool Spc700Output::advance_envelope(Voice& v) const {
    const auto& s = *v.zone;
    const float rate = static_cast<float>(m_sample_rate);

    // How much of the remaining distance to close each frame, for a stage that
    // is meant to take `seconds`. See the note on Decay below for why these
    // stages are exponential and Attack is not.
    const auto approach = [rate](float seconds) {
        // kSettle time constants is where "it has arrived" is drawn: about one
        // percent of the way left, which is -40dB and inaudible under anything.
        constexpr float kSettle = 4.6f;
        return seconds <= 0.0f ? 1.0f : 1.0f - std::exp(-kSettle / (seconds * rate));
    };

    switch (v.stage) {
        case Stage::Attack:
            // Linear, which is what both the chip and the SoundFont
            // specification do: an attack that approached its peak
            // asymptotically would never quite start the note.
            v.envelope += s.attack <= 0.0f ? 1.0f : 1.0f / (s.attack * rate);
            if (v.envelope >= 1.0f) { v.envelope = 1.0f; v.stage = Stage::Decay; }
            break;

        case Stage::Decay:
            // ── Exponential, not linear ──────────────────────────────────────
            //
            // The chip's decay subtracts a proportion of what is left rather
            // than a fixed step, and the SoundFont specification says the same
            // thing in its own words -- its envelope times are linear in
            // decibels, which is exponential in amplitude. A linear fall is
            // wrong for both, and wrong in an audible way: it holds the note up
            // too long and then arrives at the sustain level abruptly, where a
            // real instrument gives up most of its energy immediately and
            // trails off.
            if (s.decay <= 0.0f) {
                v.envelope = s.sustain;
                v.stage = Stage::Sustain;
            } else {
                v.envelope += (s.sustain - v.envelope) * approach(s.decay);
                // Within a hair of the target is the target; an exponential
                // never actually reaches it.
                if (v.envelope - s.sustain <= 0.001f) {
                    v.envelope = s.sustain;
                    v.stage = Stage::Sustain;
                }
            }
            break;

        case Stage::Sustain:
            // The chip decays here too, at a rate of its own, and a rip states
            // it: register $x6's low five bits, mapped through the documented
            // table. Zero is the chip's "infinite" and a SoundFont's silence on
            // the subject, and both mean hold.
            //
            // Exponential like the other falling stages, so a held note thins
            // out rather than ramping to nothing.
            if (s.sustain_rate > 0.0f) v.envelope -= v.envelope * approach(s.sustain_rate);
            if (v.envelope <= kSilence) return false;
            break;

        case Stage::Release:
            // Exponential for the same reason as decay. The chip's release is
            // the one place it goes linear; a note that ends by sliding
            // straight to zero clicks, and the specification's curve is the
            // better behaviour to share with SoundFont banks.
            v.envelope -= v.envelope * approach(s.release);
            if (v.envelope <= kSilence) return false;
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
        // What reaches the echo, which is not always everything.
        float send_left = 0.0f;
        float send_right = 0.0f;
        for (auto& v : m_voices) {
            if (!v.active || !v.sample || !v.zone) continue;

            if (!advance_envelope(v)) {
                v = {};   // releases this voice's hold on its bank
                continue;
            }

            const auto& s = *v.sample;
            const auto& z = *v.zone;
            const float raw = sample_at(s, v.position) * v.envelope * v.level * kVoiceGain;
            v.position += v.rate;

            if (z.loop_start >= 0 && z.loop_end > z.loop_start) {
                if (v.position >= z.loop_end) {
                    const double span = static_cast<double>(z.loop_end - z.loop_start);
                    v.position = z.loop_start + std::fmod(v.position - z.loop_end, span);
                }
            } else if (v.position >= static_cast<double>(s.data.size())) {
                // Ran off the end of a one-shot. Not a release: there is simply
                // nothing left to read.
                v = {};
                continue;
            }

            const auto& ch = m_channels[v.channel];
            const float amp = raw * ch.volume;
            const float to_left = amp * std::sqrt(1.0f - ch.pan);
            const float to_right = amp * std::sqrt(ch.pan);
            left  += to_left;
            right += to_right;
            if (z.echo_send) {
                send_left  += to_left;
                send_right += to_right;
            }
        }

        // After the voices and before the output clip, which is where the chip
        // puts it: the echo carries what was played, and what comes back is
        // subject to the same ceiling as everything else.
        if (m_block_bank && m_block_bank->echo.enabled) {
            apply_echo(m_block_bank->echo, left, right, send_left, send_right);
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
        for (const auto& program : bank->programs) {
            for (const auto& zone : program.zones) longest = std::max(longest, zone.release);
        }
        if (bank->echo.enabled) {
            // The echo has to run out too, or a rendered file ends by cutting
            // the tail off the last chord. How long it takes depends on the
            // feedback: at 0.5 a repeat is inaudible after a handful of passes,
            // at 0.9 it takes dozens.
            const float passes = bank->echo.feedback >= 0.99f
                                     ? 40.0f
                                     : std::min(40.0f, 7.0f / (1.0f - std::abs(bank->echo.feedback)));
            longest = std::max(longest, passes * bank->echo.delay_ms / 1000.0f);
        }
    }
    return static_cast<int>(longest * m_sample_rate) + m_sample_rate / 10;
}

} // namespace midi_composer::playback

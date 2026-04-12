#include "playback_engine.hpp"
#include "base/logger.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace midi_composer::playback {

PlaybackEngine::PlaybackEngine(device::MidiService& midi_service)
    : m_midi_service(midi_service) {
    m_midi_service.set_input_callback([this](const std::vector<unsigned char>& msg, double ts) {
        this->handle_incoming_midi(msg, ts);
    });
    m_thread = std::thread(&PlaybackEngine::thread_proc, this);
}

PlaybackEngine::~PlaybackEngine() {
    shutdown();
}

void PlaybackEngine::shutdown() {
    m_state = TransportState::Stopped;
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // Thread is gone; silence anything still sounding.
    std::lock_guard lock(m_state_mutex);
    all_notes_off_locked();
}

void PlaybackEngine::build_snapshot(const project::ProjectDocument& doc) {
    std::lock_guard lock(m_state_mutex);

    const auto& comp = doc.composition();
    m_ppqn = comp.ppqn();

    m_tempo.clear();
    for (const auto& ev : comp.tempo_map().events()) {
        m_tempo.push_back({ev.tick.value(), ev.microseconds_per_quarter});
    }
    std::sort(m_tempo.begin(), m_tempo.end(),
              [](const auto& a, const auto& b) { return a.tick < b.tick; });

    m_meter.clear();
    for (const auto& ev : comp.time_signature_map().events()) {
        m_meter.push_back({ev.tick.value(), ev.numerator, ev.denominator});
    }
    std::sort(m_meter.begin(), m_meter.end(),
              [](const auto& a, const auto& b) { return a.tick < b.tick; });

    const bool any_solo = std::any_of(comp.tracks().begin(), comp.tracks().end(),
                                      [](const auto& t) { return t.is_solo(); });

    m_notes.clear();
    m_programs.clear();
    for (const auto& track : comp.tracks()) {
        if (track.is_muted()) continue;
        if (any_solo && !track.is_solo()) continue;
        const uint8_t channel = track.midi_channel() & 0x0F;
        for (const auto& note : track.notes()) {
            m_notes.push_back({channel, note.pitch, note.velocity,
                               note.start.value(), note.end().value()});
        }
        for (const auto& pc : track.program_changes()) {
            m_programs.push_back({pc.tick.value(), channel, pc.program});
        }
    }
    std::sort(m_notes.begin(), m_notes.end(),
              [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
    std::sort(m_programs.begin(), m_programs.end(),
              [](const auto& a, const auto& b) { return a.tick < b.tick; });

    // Notes are the only thing the scheduler plays, so they alone decide when
    // the composition has run out: a program change past the last note produces
    // no sound and is not worth waiting for. Sorted by start, not by end, so
    // this has to be a scan rather than a look at the back.
    m_content_end_tick = 0;
    for (const auto& note : m_notes) {
        if (note.end_tick > note.start_tick) {
            m_content_end_tick = std::max(m_content_end_tick, note.end_tick);
        }
    }
}

void PlaybackEngine::refresh_snapshot(const project::ProjectDocument& doc) {
    build_snapshot(doc);
}

void PlaybackEngine::play(const project::ProjectDocument& doc) {
    build_snapshot(doc);
    {
        std::lock_guard lock(m_state_mutex);
        m_last_metronome_tick = -1;
        send_effective_programs_locked(static_cast<int64_t>(m_precise_tick));
    }
    m_state = TransportState::Playing;
    notify_state(TransportState::Playing, current_tick());
    MC_LOG_INFO("Playback started");
}

void PlaybackEngine::record(const project::ProjectDocument& doc) {
    build_snapshot(doc);
    {
        std::lock_guard lock(m_state_mutex);
        m_last_metronome_tick = -1;
        send_effective_programs_locked(static_cast<int64_t>(m_precise_tick));
    }
    m_state = TransportState::Recording;
    notify_state(TransportState::Recording, current_tick());
    MC_LOG_INFO("Recording started");
}

void PlaybackEngine::stop() {
    m_state = TransportState::Stopped;
    {
        std::lock_guard lock(m_state_mutex);
        m_precise_tick = 0.0;
        m_current_tick = 0;
        m_last_metronome_tick = -1;
        all_notes_off_locked();
    }
    {
        std::lock_guard lock(m_recording_mutex);
        m_pending_notes.clear();
    }
    // Let the UI know the cursor went back to zero and the buttons should follow.
    notify_position(timeline::Tick{0});
    notify_state(TransportState::Stopped, timeline::Tick{0});
    MC_LOG_INFO("Playback stopped");
}

void PlaybackEngine::pause() {
    auto current = m_state.load();
    if (current == TransportState::Playing || current == TransportState::Recording) {
        m_state = TransportState::Paused;
        {
            std::lock_guard lock(m_state_mutex);
            all_notes_off_locked();
        }
        notify_state(TransportState::Paused, current_tick());
        MC_LOG_INFO("Playback paused");
    }
}

void PlaybackEngine::notify_position(timeline::Tick tick) {
    PositionCallback cb;
    {
        std::lock_guard lock(m_callback_mutex);
        cb = m_position_callback;
    }
    if (cb) cb(tick);
}

void PlaybackEngine::notify_state(TransportState state, timeline::Tick tick) {
    StateCallback cb;
    {
        std::lock_guard lock(m_callback_mutex);
        cb = m_state_callback;
    }
    if (cb) cb(state, tick);
}

void PlaybackEngine::seek(timeline::Tick tick) {
    const int64_t target = std::max<int64_t>(0, tick.value());
    std::lock_guard lock(m_state_mutex);
    m_precise_tick = static_cast<double>(target);
    m_current_tick = target;
    m_last_metronome_tick = -1;
    all_notes_off_locked();
    send_effective_programs_locked(target);
    MC_LOG_DEBUG("Seek to tick: {}", target);
}

double PlaybackEngine::advance_ticks(double start_tick, double elapsed_us) const {
    constexpr uint32_t kDefaultUsPerQuarter = 500000; // 120 BPM
    double tick = start_tick;
    double remaining = elapsed_us;

    // Index of the tempo segment in effect at `tick`.
    size_t idx = 0;
    while (idx + 1 < m_tempo.size() && static_cast<double>(m_tempo[idx + 1].tick) <= tick) {
        ++idx;
    }

    while (remaining > 0.0) {
        const uint32_t uspq = m_tempo.empty() ? kDefaultUsPerQuarter
                                              : m_tempo[idx].microseconds_per_quarter;
        const double ticks_per_us = static_cast<double>(m_ppqn) / static_cast<double>(uspq);
        const bool has_next = idx + 1 < m_tempo.size();
        if (!has_next) {
            tick += remaining * ticks_per_us;
            break;
        }
        const double next_tick = static_cast<double>(m_tempo[idx + 1].tick);
        const double us_to_next = (next_tick - tick) / ticks_per_us;
        if (us_to_next > remaining) {
            tick += remaining * ticks_per_us;
            break;
        }
        tick = next_tick;
        remaining -= us_to_next;
        ++idx;
    }
    return tick;
}

void PlaybackEngine::process_metronome(int64_t start_tick, int64_t end_tick) {
    // Effective meter at start_tick (default 4/4).
    MeterSegment seg{0, 4, 4};
    for (const auto& m : m_meter) {
        if (m.tick <= start_tick) seg = m;
        else break;
    }
    if (seg.denominator == 0 || seg.numerator == 0) return;

    const int64_t beat_ticks = m_ppqn * 4 / seg.denominator;
    if (beat_ticks <= 0) return;

    // First beat at or after start_tick, counted from the meter change point.
    const int64_t rel = std::max<int64_t>(0, start_tick - seg.tick);
    const int64_t k = (rel + beat_ticks - 1) / beat_ticks;
    const int64_t beat_tick = seg.tick + k * beat_ticks;

    if (beat_tick >= start_tick && beat_tick < end_tick && beat_tick != m_last_metronome_tick) {
        const bool is_downbeat = (k % seg.numerator) == 0;
        const uint8_t pitch = is_downbeat ? 77 : 76; // GM wood blocks
        send_note_on(9, pitch, is_downbeat ? 110 : 90);
        // Schedule the matching note-off through the regular machinery.
        m_playing_notes.push_back({9, pitch, beat_tick + beat_ticks / 2});
        m_last_metronome_tick = beat_tick;
    }
}

void PlaybackEngine::thread_proc() {
    using namespace std::chrono_literals;
    auto last_time = std::chrono::steady_clock::now();
    auto last_notify_time = last_time;

    while (m_running) {
        auto now = std::chrono::steady_clock::now();
        auto delta = now - last_time;
        last_time = now;

        auto state = m_state.load();
        if (state == TransportState::Playing || state == TransportState::Recording) {
            const double elapsed_us =
                std::chrono::duration<double, std::micro>(delta).count();
            bool reached_end = false;

            {
                std::lock_guard lock(m_state_mutex);

                const double start = m_precise_tick;
                const double end = advance_ticks(start, elapsed_us);
                m_precise_tick = end;

                const int64_t start_tick = static_cast<int64_t>(start);
                const int64_t end_tick = static_cast<int64_t>(end);
                m_current_tick = end_tick;

                // 0. Metronome click (playback and recording)
                if (m_metronome_enabled && m_midi_service.is_output_open()) {
                    process_metronome(start_tick, end_tick);
                }

                // 1. NoteOffs for notes that ended in this slice (also flushes
                //    metronome clicks while recording)
                auto it = m_playing_notes.begin();
                while (it != m_playing_notes.end()) {
                    if (it->end_tick <= end_tick) {
                        send_note_off(it->channel, it->pitch);
                        it = m_playing_notes.erase(it);
                    } else {
                        ++it;
                    }
                }

                // 2. Program changes landing in this slice, before any NoteOn
                //    that depends on them.
                for (const auto& pc : m_programs) {
                    if (pc.tick >= end_tick) break;  // sorted by tick
                    if (pc.tick >= start_tick) send_program_change(pc.channel, pc.program);
                }

                // 3. NoteOns for snapshot notes starting in this slice. Also
                //    active while recording so existing material is audible.
                for (const auto& note : m_notes) {
                    if (note.start_tick >= end_tick) break; // sorted by start
                    if (note.start_tick >= start_tick && note.end_tick > note.start_tick) {
                        send_note_on(note.channel, note.pitch, note.velocity);
                        m_playing_notes.push_back({note.channel, note.pitch, note.end_tick});
                    }
                }

                // 4. Nothing left ahead and nothing still sounding: the
                //    composition is over. Playing only — while recording the
                //    whole point is to keep running and wait for input — and
                //    only when something was actually scheduled, so an empty
                //    document still runs as a bare click track instead of
                //    stopping the instant it starts.
                reached_end = state == TransportState::Playing
                              && m_content_end_tick > 0
                              && end_tick >= m_content_end_tick
                              && m_playing_notes.empty();
            }

            if (reached_end) {
                // stop() takes m_state_mutex, so it must be called from out here.
                MC_LOG_INFO("Reached end of composition; stopping");
                stop();
            } else if (now - last_notify_time > 33ms) {
                // Throttled position notification (approx 30Hz)
                notify_position(timeline::Tick{m_current_tick.load()});
                last_notify_time = now;
            }
        }

        std::this_thread::sleep_for(5ms);
    }
}

void PlaybackEngine::handle_incoming_midi(const std::vector<unsigned char>& message, double) {
    if (message.size() < 3) return;
    if (m_state != TransportState::Recording) return;

    const uint8_t status = message[0] & 0xF0;
    const uint8_t channel = message[0] & 0x0F;

    if (status != 0x90 && status != 0x80) return;

    const uint8_t pitch = message[1] & 0x7F;
    const uint8_t velocity = (status == 0x90) ? (message[2] & 0x7F) : 0;
    const uint16_t key = (static_cast<uint16_t>(channel) << 8) | pitch;

    if (velocity > 0) {
        // Note On
        std::lock_guard lock(m_recording_mutex);
        m_pending_notes[key] = {channel, pitch, velocity, m_current_tick.load()};
        return;
    }

    // Note Off (0x80, or 0x90 with velocity 0)
    PendingNote pending{};
    {
        std::lock_guard lock(m_recording_mutex);
        auto it = m_pending_notes.find(key);
        if (it == m_pending_notes.end()) return;
        pending = it->second;
        m_pending_notes.erase(it);
    }

    int64_t duration = m_current_tick.load() - pending.start_tick;
    if (duration < 10) duration = 10; // Minimum duration

    // Hand the completed note to the document owner; it allocates the note id
    // and performs the edit under its own lock.
    NoteCommitCallback commit;
    {
        std::lock_guard lock(m_callback_mutex);
        commit = m_note_commit_callback;
    }
    if (commit) {
        commit(pending.pitch, pending.velocity, pending.start_tick, duration);
    } else {
        MC_LOG_WARN("Recorded note dropped: no commit callback installed");
    }
}

void PlaybackEngine::send_program_change(uint8_t channel, uint8_t program) {
    if (channel > 15) channel = 0;
    unsigned char msg[2];
    msg[0] = static_cast<unsigned char>(0xC0 | channel);
    msg[1] = program & 0x7F;
    m_midi_service.send_message(msg, 2);
}

void PlaybackEngine::send_effective_programs_locked(int64_t tick) {
    if (!m_midi_service.is_output_open()) return;
    // m_programs is sorted by tick, so walking forward and overwriting leaves
    // the last program at or before `tick` for each channel.
    uint8_t effective[16];
    bool has[16] = {};
    for (const auto& pc : m_programs) {
        if (pc.tick > tick) break;
        const uint8_t ch = pc.channel & 0x0F;
        effective[ch] = pc.program;
        has[ch] = true;
    }
    for (uint8_t ch = 0; ch < 16; ++ch) {
        if (has[ch]) send_program_change(ch, effective[ch]);
    }
}

void PlaybackEngine::send_note_on(uint8_t channel, uint8_t pitch, uint8_t velocity) {
    if (channel > 15) channel = 0;
    unsigned char msg[3];
    msg[0] = static_cast<unsigned char>(0x90 | channel);
    msg[1] = pitch & 0x7F;
    msg[2] = velocity & 0x7F;
    m_midi_service.send_message(msg, 3);
}

void PlaybackEngine::send_note_off(uint8_t channel, uint8_t pitch) {
    if (channel > 15) channel = 0;
    unsigned char msg[3];
    msg[0] = static_cast<unsigned char>(0x80 | channel);
    msg[1] = pitch & 0x7F;
    msg[2] = 0;
    m_midi_service.send_message(msg, 3);
}

void PlaybackEngine::all_notes_off_locked() {
    for (const auto& note : m_playing_notes) {
        send_note_off(note.channel, note.pitch);
    }
    m_playing_notes.clear();
}

} // namespace midi_composer::playback

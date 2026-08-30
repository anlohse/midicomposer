#include "playback_engine.hpp"
#include "base/logger.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace midi_composer::playback {

PlaybackEngine::PlaybackEngine(device::MidiService& midi_input, OutputPlugin& output)
    : m_midi_input(midi_input), m_output(&output) {
    m_midi_input.set_input_callback([this](const std::vector<unsigned char>& msg, double ts) {
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

namespace {
// The clock OutputPlugin timestamps are on. Microseconds rather than a chrono
// type because the interface is shaped for a boundary those cannot cross.
int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The two channel-mode controllers the mixer maps onto.
constexpr uint8_t kControllerVolume = 7;
constexpr uint8_t kControllerPan    = 10;

// MIDI has no master volume, so the master fader is applied here, to each
// channel's volume on its way out. Rounded rather than truncated so a master
// just below unity does not quietly drop every channel a step.
uint8_t scale_by_master(uint8_t volume, uint8_t master) {
    if (master >= 127) return volume;   // unity: leave the fader's own value alone
    return static_cast<uint8_t>((static_cast<int>(volume) * master + 63) / 127);
}
} // namespace

void PlaybackEngine::build_snapshot(const project::ProjectDocument& doc) {
    std::lock_guard lock(m_state_mutex);

    const auto& comp = doc.composition();
    m_ppqn = comp.ppqn();
    m_master_volume = comp.master_volume();

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
    m_controllers.clear();
    m_bends.clear();
    for (auto& mix : m_mix) mix.reset();
    for (const auto& track : comp.tracks()) {
        if (track.is_muted()) continue;
        if (any_solo && !track.is_solo()) continue;
        const uint8_t channel = track.midi_channel() & 0x0F;
        // Two tracks can share a channel, and a channel has one volume: the
        // last one wins rather than the two fighting over it every refresh.
        m_mix[channel] = ChannelMix{track.volume(), track.pan()};
        for (const auto& note : track.notes()) {
            m_notes.push_back({channel, note.pitch, note.velocity,
                               note.start.value(), note.end().value()});
        }
        for (const auto& pc : track.program_changes()) {
            m_programs.push_back({pc.tick.value(), channel, pc.program});
        }
        for (const auto& cc : track.controller_events()) {
            m_controllers.push_back({cc.tick.value(), channel, cc.controller, cc.value});
        }
        for (const auto& pb : track.pitch_bends()) {
            m_bends.push_back({pb.tick.value(), channel, pb.value});
        }
    }
    std::sort(m_notes.begin(), m_notes.end(),
              [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
    std::sort(m_programs.begin(), m_programs.end(),
              [](const auto& a, const auto& b) { return a.tick < b.tick; });
    std::stable_sort(m_controllers.begin(), m_controllers.end(),
                     [](const auto& a, const auto& b) { return a.tick < b.tick; });
    std::stable_sort(m_bends.begin(), m_bends.end(),
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
    // Mute and solo change which channels are audible at all, so the mix has to
    // follow an edit and not wait for the next play. Only the difference goes
    // out, so an edit that left the mixer alone sends nothing.
    const auto state = m_state.load();
    if (state == TransportState::Playing || state == TransportState::Recording) {
        std::lock_guard lock(m_state_mutex);
        send_mix_locked(false, steady_us());
    }
}

void PlaybackEngine::set_channel_mix(uint8_t channel, uint8_t volume, uint8_t pan) {
    channel &= 0x0F;
    std::lock_guard lock(m_state_mutex);
    // Only when the channel is already in the snapshot: a muted track is absent
    // from it, and moving its fader must not make it audible again.
    if (!m_mix[channel]) return;
    m_mix[channel] = ChannelMix{volume, pan};
    const auto state = m_state.load();
    if (state == TransportState::Playing || state == TransportState::Recording) {
        send_mix_locked(false, steady_us());
    }
}

std::optional<ChannelMix> PlaybackEngine::channel_mix(uint8_t channel) const {
    std::lock_guard lock(m_state_mutex);
    return m_mix[channel & 0x0F];
}

void PlaybackEngine::set_master_volume(uint8_t volume) {
    std::lock_guard lock(m_state_mutex);
    m_master_volume = volume > 127 ? 127 : volume;
    const auto state = m_state.load();
    if (state == TransportState::Playing || state == TransportState::Recording) {
        send_mix_locked(false, steady_us());
    }
}

std::optional<uint8_t> PlaybackEngine::effective_volume(uint8_t channel) const {
    std::lock_guard lock(m_state_mutex);
    const auto& mix = m_mix[channel & 0x0F];
    if (!mix) return std::nullopt;
    return scale_by_master(mix->volume, m_master_volume);
}

void PlaybackEngine::send_mix_locked(bool force, int64_t now_us) {
    if (!m_output_started) return;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        if (!m_mix[ch]) continue;
        // Pan is left alone: the master is a level, and scaling a position
        // would drag every channel towards the left as it came down.
        const ChannelMix wire{scale_by_master(m_mix[ch]->volume, m_master_volume),
                              m_mix[ch]->pan};
        if (!force && m_sent_mix[ch] == wire) continue;
        send_controller(ch, kControllerVolume, wire.volume, now_us);
        send_controller(ch, kControllerPan, wire.pan, now_us);
        m_sent_mix[ch] = wire;
    }
}

base::Result<void> PlaybackEngine::play(const project::ProjectDocument& doc) {
    // Before anything else, and outside the lock: this is where a plugin opens
    // its device or loads what it needs, and it is allowed to take a moment.
    if (auto started = m_output->start(); !started) {
        MC_LOG_WARN("Playback refused: {}", started.error().message);
        return started;
    }
    build_snapshot(doc);
    {
        std::lock_guard lock(m_state_mutex);
        m_output_started = true;
        m_last_metronome_tick = -1;
        send_effective_state_locked(static_cast<int64_t>(m_precise_tick), steady_us());
    }
    m_state = TransportState::Playing;
    notify_state(TransportState::Playing, current_tick());
    MC_LOG_INFO("Playback started");
    return {};
}

base::Result<void> PlaybackEngine::record(const project::ProjectDocument& doc) {
    if (auto started = m_output->start(); !started) {
        MC_LOG_WARN("Recording refused: {}", started.error().message);
        return started;
    }
    build_snapshot(doc);
    {
        std::lock_guard lock(m_state_mutex);
        m_output_started = true;
        m_last_metronome_tick = -1;
        send_effective_state_locked(static_cast<int64_t>(m_precise_tick), steady_us());
    }
    m_state = TransportState::Recording;
    notify_state(TransportState::Recording, current_tick());
    MC_LOG_INFO("Recording started");
    return {};
}

void PlaybackEngine::stop() {
    m_state = TransportState::Stopped;
    {
        std::lock_guard lock(m_state_mutex);
        m_precise_tick = 0.0;
        m_current_tick = 0;
        m_last_metronome_tick = -1;
        // Silence first, then stop expecting events: clearing the flag before
        // the note-offs would leave everything sounding hang.
        all_notes_off_locked();
        m_output_started = false;
    }
    m_output->stop();
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
    send_effective_state_locked(target, steady_us());
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
        send_note_on(9, pitch, is_downbeat ? 110 : 90, due_us_locked(static_cast<double>(beat_tick)));
        // Schedule the matching note-off through the regular machinery.
        m_playing_notes.push_back({9, pitch, beat_tick + beat_ticks / 2});
        m_last_metronome_tick = beat_tick;
    }
}

void PlaybackEngine::dispatch_slice_locked(int64_t start_tick, int64_t end_tick,
                                           bool metronome) {
    // 0. Metronome click (playback and recording)
    if (metronome && m_metronome_enabled && m_output_started) {
        process_metronome(start_tick, end_tick);
    }

    // 1. NoteOffs for notes that ended in this slice (also flushes
    //    metronome clicks while recording)
    auto it = m_playing_notes.begin();
    while (it != m_playing_notes.end()) {
        if (it->end_tick <= end_tick) {
            send_note_off(it->channel, it->pitch,
                          due_us_locked(static_cast<double>(it->end_tick)));
            it = m_playing_notes.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Program changes landing in this slice, before any NoteOn
    //    that depends on them.
    for (const auto& pc : m_programs) {
        if (pc.tick >= end_tick) break;  // sorted by tick
        if (pc.tick >= start_tick) {
            send_program_change(pc.channel, pc.program,
                                due_us_locked(static_cast<double>(pc.tick)));
        }
    }

    // 2b. Controllers and pitch bends landing in this slice, before
    //     the NoteOns below: a volume or bend written at the same
    //     tick as a note is meant to apply to that note.
    for (const auto& cc : m_controllers) {
        if (cc.tick >= end_tick) break;      // sorted by tick
        if (cc.tick >= start_tick) {
            send_controller(cc.channel, cc.controller, cc.value,
                            due_us_locked(static_cast<double>(cc.tick)));
        }
    }
    for (const auto& pb : m_bends) {
        if (pb.tick >= end_tick) break;
        if (pb.tick >= start_tick) {
            send_pitch_bend(pb.channel, pb.value,
                            due_us_locked(static_cast<double>(pb.tick)));
        }
    }

    // 3. NoteOns for snapshot notes starting in this slice. Also
    //    active while recording so existing material is audible.
    for (const auto& note : m_notes) {
        if (note.start_tick >= end_tick) break; // sorted by start
        if (note.start_tick >= start_tick && note.end_tick > note.start_tick) {
            send_note_on(note.channel, note.pitch, note.velocity,
                         due_us_locked(static_cast<double>(note.start_tick)));
            m_playing_notes.push_back({note.channel, note.pitch, note.end_tick});
        }
    }
}

base::Result<RenderedAudio> PlaybackEngine::render_offline(const project::ProjectDocument& doc) {
    AudioSource* source = m_output->audio();
    if (!source) {
        return std::unexpected(base::Error{
            base::ErrorCode::InvalidState,
            "The selected output does not produce audio (" + std::string(m_output->name()) + ")"});
    }
    const auto state = m_state.load();
    if (state == TransportState::Playing || state == TransportState::Recording) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState,
                                           "Stop playback before rendering"});
    }

    if (auto started = m_output->start(); !started) return std::unexpected(started.error());

    build_snapshot(doc);

    const int rate = source->sample_rate();
    if (rate <= 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState,
                                           "The output reported no sample rate"});
    }

    // Small enough that events land close to where they belong even before the
    // timestamps place them, large enough not to pay the per-block cost too
    // often.
    constexpr int kBlockFrames = 64;
    const double block_us = 1'000'000.0 * kBlockFrames / rate;

    RenderedAudio out;
    out.sample_rate = rate;

    std::vector<float> block(static_cast<size_t>(kBlockFrames) * 2);

    std::lock_guard lock(m_state_mutex);
    // The renderer stands in for the transport for the duration, so the same
    // send path is open. Restored below whatever happens.
    const bool was_started = m_output_started;
    m_output_started = true;
    m_playing_notes.clear();
    m_last_metronome_tick = -1;
    // The same state a play would establish -- program changes, the mixer,
    // anything written before the first slice -- stamped on the render's clock
    // rather than the wall clock, which here would be permanently in the future.
    send_effective_state_locked(0, 0);

    const int64_t content_end = m_content_end_tick;
    double tick = 0.0;
    int64_t elapsed_us = 0;

    // A cap rather than a trust: a tempo of nearly zero would otherwise render
    // until the disk filled.
    constexpr int64_t kMaxFrames = 60LL * 60 * 192000;

    while (static_cast<int64_t>(out.interleaved_stereo.size() / 2) < kMaxFrames) {
        const double next = advance_ticks(tick, block_us);

        m_slice_base_tick = tick;
        m_slice_tick_span = next - tick;
        m_slice_span_us   = block_us;
        m_slice_base_us   = elapsed_us;

        dispatch_slice_locked(static_cast<int64_t>(tick), static_cast<int64_t>(next),
                              /*metronome*/ false);

        source->begin_block(elapsed_us);
        source->render(block.data(), kBlockFrames);
        out.interleaved_stereo.insert(out.interleaved_stereo.end(), block.begin(), block.end());

        tick = next;
        elapsed_us += static_cast<int64_t>(block_us);

        if (static_cast<int64_t>(tick) >= content_end && m_playing_notes.empty()) break;
    }

    // The tail the output asks for: releases and echo have to decay, or the
    // file ends on a click.
    const int tail = source->tail_frames();
    for (int rendered = 0; rendered < tail; rendered += kBlockFrames) {
        elapsed_us += static_cast<int64_t>(block_us);
        source->begin_block(elapsed_us);
        source->render(block.data(), kBlockFrames);
        out.interleaved_stereo.insert(out.interleaved_stereo.end(), block.begin(), block.end());
    }

    m_output_started = was_started;
    m_playing_notes.clear();
    return out;
}

void PlaybackEngine::set_output(OutputPlugin& output) {
    std::lock_guard lock(m_state_mutex);
    m_output = &output;
    // Whatever the previous output was told is not true of this one.
    for (auto& sent : m_sent_mix) sent.reset();
    m_playing_notes.clear();
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

                // Ticks [start, end] cover the wall clock this iteration just
                // measured, which is what lets an event's due time be recovered
                // from its tick.
                m_slice_base_tick = start;
                m_slice_tick_span = end - start;
                m_slice_span_us   = elapsed_us;
                m_slice_base_us   = steady_us() - static_cast<int64_t>(elapsed_us);

                dispatch_slice_locked(start_tick, end_tick, /*metronome*/ true);

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

            // Once per iteration rather than per event: an output fails for one
            // reason, the device going away, and that is a sticky condition.
            // Checked out here because stop() takes m_state_mutex.
            if (auto failed = m_output->failure()) {
                MC_LOG_ERROR("Output failed, stopping playback: {}", failed->message);
                stop();
            } else if (reached_end) {
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

void PlaybackEngine::send_program_change(uint8_t channel, uint8_t program, int64_t when_us) {
    if (!m_output_started) return;
    m_output->program_change(channel, program, when_us);
}

void PlaybackEngine::send_controller(uint8_t channel, uint8_t controller, uint8_t value,
                                     int64_t when_us) {
    if (!m_output_started) return;
    m_output->controller(channel, controller, value, when_us);
}

void PlaybackEngine::send_pitch_bend(uint8_t channel, int16_t value, int64_t when_us) {
    if (!m_output_started) return;
    // The 14-bit wire encoding moved to the plugin, where the bytes are: an
    // implementation that is not a MIDI cable has no use for it.
    m_output->pitch_bend(channel, value, when_us);
}

void PlaybackEngine::send_effective_state_locked(int64_t tick, int64_t now_us) {
    if (!m_output_started) return;

    // Each list is sorted by tick, so walking forward and overwriting leaves the
    // last value at or before `tick` for each channel. Starting or seeking into
    // the middle then sounds the way playing up to that point would have —
    // without this, a volume controller written in bar 1 would simply never be
    // sent when playback starts from bar 20.
    uint8_t program[16];
    bool has_program[16] = {};
    for (const auto& pc : m_programs) {
        if (pc.tick > tick) break;
        const uint8_t ch = pc.channel & 0x0F;
        program[ch] = pc.program;
        has_program[ch] = true;
    }
    for (uint8_t ch = 0; ch < 16; ++ch) {
        if (has_program[ch]) send_program_change(ch, program[ch], now_us);
    }

    // The mixer first, so a CC 7 or CC 10 written into the score below still
    // wins from its own tick onward: the fader is the channel's starting point,
    // not an override of the music. Forced, because starting or seeking has to
    // establish the channel state whatever was last sent.
    send_mix_locked(true, now_us);

    // Per (channel, controller) rather than per channel: a track can set volume,
    // pan and modulation independently, and only the latest of each applies.
    std::map<std::pair<uint8_t, uint8_t>, uint8_t> controllers;
    for (const auto& cc : m_controllers) {
        if (cc.tick > tick) break;
        controllers[{cc.channel & 0x0F, cc.controller}] = cc.value;
    }
    for (const auto& [key, value] : controllers) {
        send_controller(key.first, key.second, value, now_us);
        // The score just overwrote what the mixer put on this channel. Forget
        // the sent value rather than the wanted one, so the fader's setting
        // returns the next time it is touched instead of being suppressed as
        // already-sent.
        if (key.second == kControllerVolume || key.second == kControllerPan) {
            m_sent_mix[key.first].reset();
        }
    }

    int16_t bend[16] = {};
    bool has_bend[16] = {};
    bool any_bend[16] = {};
    for (const auto& pb : m_bends) {
        const uint8_t ch = pb.channel & 0x0F;
        any_bend[ch] = true;
        if (pb.tick > tick) continue;   // still need the scan for any_bend
        bend[ch] = pb.value;
        has_bend[ch] = true;
    }
    for (uint8_t ch = 0; ch < 16; ++ch) {
        // A channel that bends somewhere but not yet is re-centred: seeking
        // backwards past a bend must not leave the pitch hanging where it was.
        if (any_bend[ch]) send_pitch_bend(ch, has_bend[ch] ? bend[ch] : 0, now_us);
    }
}

void PlaybackEngine::send_note_on(uint8_t channel, uint8_t pitch, uint8_t velocity,
                                  int64_t when_us) {
    if (!m_output_started) return;
    m_output->note_on(channel, pitch, velocity, when_us);
}

void PlaybackEngine::send_note_off(uint8_t channel, uint8_t pitch, int64_t when_us) {
    if (!m_output_started) return;
    m_output->note_off(channel, pitch, when_us);
}

int64_t PlaybackEngine::due_us_locked(double tick) const {
    // Outside a slice -- or a slice of no length -- there is nothing to
    // interpolate against, and the event is simply due now.
    if (m_slice_tick_span <= 0.0) return steady_us();
    const double frac =
        std::clamp((tick - m_slice_base_tick) / m_slice_tick_span, 0.0, 1.0);
    return m_slice_base_us + static_cast<int64_t>(frac * m_slice_span_us);
}

void PlaybackEngine::all_notes_off_locked() {
    const int64_t now_us = steady_us();
    for (const auto& note : m_playing_notes) {
        send_note_off(note.channel, note.pitch, now_us);
    }
    m_playing_notes.clear();
}

} // namespace midi_composer::playback

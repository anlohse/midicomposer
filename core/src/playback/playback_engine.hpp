#pragma once

#include "playback/transport_state.hpp"
#include "timeline/tick.hpp"
#include "project/project_document.hpp"
#include "device/midi_service.hpp"
#include "playback/output_plugin.hpp"
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include <map>

namespace midi_composer::playback {

// Immutable copies of the document data the playback thread needs. They are
// rebuilt on the caller's thread by play()/record()/refresh_snapshot() so the
// playback thread never dereferences the live document (threading rule: the
// scheduler must not iterate raw domain data that another thread may mutate).
struct PlaybackNoteEvent {
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    int64_t start_tick;
    int64_t end_tick;
};

struct TempoSegment {
    int64_t tick;
    uint32_t microseconds_per_quarter;
};

struct MeterSegment {
    int64_t tick;
    uint8_t numerator;
    uint8_t denominator;
};

// A track's instrument is the program change at tick 0, so these must be sent
// for the chosen instrument to be audible at all.
struct PlaybackProgramEvent {
    int64_t tick;
    uint8_t channel;
    uint8_t program;
};

struct PlaybackControllerEvent {
    int64_t tick;
    uint8_t channel;
    uint8_t controller;
    uint8_t value;
};

// A track's mixer position, as it reaches the MIDI channel: CC 7 and CC 10.
struct ChannelMix {
    uint8_t volume;
    uint8_t pan;

    friend bool operator==(const ChannelMix&, const ChannelMix&) = default;
};

struct PlaybackBendEvent {
    int64_t tick;
    uint8_t channel;
    int16_t value;   // -8192..8191, centre is 0
};

struct RenderedAudio {
    int sample_rate{0};
    std::vector<float> interleaved_stereo;
};

class PlaybackEngine {
public:
    using PositionCallback = std::function<void(timeline::Tick)>;
    // (pitch, velocity, start_tick, duration_ticks) — invoked from the MIDI
    // input thread when a recorded note is completed. The receiver owns
    // document access/locking and note-id allocation.
    using NoteCommitCallback = std::function<void(uint8_t, uint8_t, int64_t, int64_t)>;
    // Fired on every transport transition, including the automatic stop at the
    // end of the composition, which no command thread would otherwise report.
    using StateCallback = std::function<void(TransportState, timeline::Tick)>;

    // Input and output are separate now: MidiService is the MIDI input the
    // recorder listens to, and where playback goes is the plugin's business.
    PlaybackEngine(device::MidiService& midi_input, OutputPlugin& output);

    /**
     * The outputs behind the plugin have changed.
     *
     * What the engine believes it has already put on the wire is true of the
     * output that received it and of nothing else, so a channel that has moved
     * would otherwise never be told its level again.
     */
    void outputs_changed();
    ~PlaybackEngine();

    // Both start the output first and report why they could not, which is what
    // the user is shown instead of pressing Play and hearing nothing.
    base::Result<void> play(const project::ProjectDocument& doc);
    base::Result<void> record(const project::ProjectDocument& doc);
    void stop();
    void pause();
    void seek(timeline::Tick tick);
    void shutdown();

    /**
     * Render the composition to audio, faster than real time and with no audio
     * device involved.
     *
     * The same snapshot and the same slice dispatch the playback thread uses,
     * driven by a clock that is counted in frames rather than measured: the
     * result is deterministic, which is what makes it testable at all.
     *
     * Fails when the selected output produces no audio -- a MIDI port has
     * nothing to render -- and when the transport is running, because the
     * render would take the snapshot out from under it.
     */
    [[nodiscard]] base::Result<RenderedAudio> render_offline(const project::ProjectDocument& doc);

    // Rebuild the playback snapshot after an edit. Must be called on the
    // thread that owns the document (the bridge/orchestration thread).
    void refresh_snapshot(const project::ProjectDocument& doc);

    // A track's mixer setting, applied to its MIDI channel straight away when
    // something is playing. Separate from refresh_snapshot because moving a
    // fader does not change the note schedule, and a range input fires on every
    // pixel of the drag: rebuilding the whole snapshot that often would sort
    // every note in the composition dozens of times a second, on the thread the
    // playback loop is contending with.
    void set_channel_mix(uint8_t channel, uint8_t volume, uint8_t pan);

    // The master fader, scaling every channel's volume. Same reasoning as
    // set_channel_mix: it moves per pixel and changes no note.
    void set_master_volume(uint8_t volume);

    // What the snapshot holds for a channel, or nullopt when no audible track
    // occupies it. For assertions; the engine itself reads the members.
    [[nodiscard]] std::optional<ChannelMix> channel_mix(uint8_t channel) const;

    // The volume actually put on the wire for a channel: its fader scaled by the
    // master. What channel_mix reports is the track's own setting, before that.
    [[nodiscard]] std::optional<uint8_t> effective_volume(uint8_t channel) const;

    [[nodiscard]] TransportState state() const { return m_state; }
    [[nodiscard]] timeline::Tick current_tick() const { return timeline::Tick{m_current_tick.load()}; }

    void set_position_callback(PositionCallback cb) {
        std::lock_guard lock(m_callback_mutex);
        m_position_callback = std::move(cb);
    }

    void set_note_commit_callback(NoteCommitCallback cb) {
        std::lock_guard lock(m_callback_mutex);
        m_note_commit_callback = std::move(cb);
    }

    void set_state_callback(StateCallback cb) {
        std::lock_guard lock(m_callback_mutex);
        m_state_callback = std::move(cb);
    }

    // Tick the last scheduled note ends on; 0 when nothing is scheduled. This
    // is what playback runs out at.
    [[nodiscard]] int64_t content_end_tick() const {
        std::lock_guard lock(m_state_mutex);
        return m_content_end_tick;
    }

    void set_metronome_enabled(bool enabled) { m_metronome_enabled = enabled; }
    [[nodiscard]] bool is_metronome_enabled() const { return m_metronome_enabled; }

private:
    void thread_proc();
    void handle_incoming_midi(const std::vector<unsigned char>& message, double timestamp);
    void build_snapshot(const project::ProjectDocument& doc);

    // Advance a tick position by elapsed wall-clock microseconds, honoring
    // every tempo segment crossed. Requires m_state_mutex to be held.
    [[nodiscard]] double advance_ticks(double start_tick, double elapsed_us) const;
    void process_metronome(int64_t start_tick, int64_t end_tick);

    // Emit the channel volume and pan the mixer asks for. `force` sends every
    // channel; otherwise only what differs from what is already on the wire, so
    // an edit during playback does not re-send settings nothing changed.
    // Requires m_state_mutex held.
    void send_mix_locked(bool force, int64_t when_us);

    // Each carries the instant the event was due; see OutputPlugin. Events
    // scheduled from a slice interpolate it (due_us_locked), everything else --
    // the state restored on a seek, a fader moving, notes silenced on stop --
    // is genuinely immediate and passes now.
    void send_note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us);
    void send_note_off(uint8_t channel, uint8_t pitch, int64_t when_us);
    void send_program_change(uint8_t channel, uint8_t program, int64_t when_us);
    void send_controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us);
    void send_pitch_bend(uint8_t channel, int16_t value, int64_t when_us);

    // When an event at `tick` was due, interpolated inside the slice the loop is
    // processing. Every event of one iteration is late by about the same amount,
    // so what this preserves is the spacing between them -- which is what a
    // chord staying together depends on. Requires m_state_mutex held.
    [[nodiscard]] int64_t due_us_locked(double tick) const;

    // Everything the snapshot schedules in [start_tick, end_tick): note offs,
    // program changes, controllers, bends, note ons, in the order they have to
    // reach an output. Shared by the playback thread and the offline renderer,
    // so a rendered file and what is heard come from one description of what
    // happens when rather than two that can drift apart.
    // Requires m_state_mutex held.
    void dispatch_slice_locked(int64_t start_tick, int64_t end_tick, bool metronome);
    void all_notes_off_locked();  // requires m_state_mutex held

    // Sends, per channel, the last program change, controller value and pitch
    // bend at or before `tick`, so starting or seeking into the middle of a
    // composition sounds the way playing up to that point would have.
    // Requires m_state_mutex held.
    void send_effective_state_locked(int64_t tick, int64_t when_us);

    // Push the position / transport state to whoever is listening. Both copy the
    // callback out and invoke it with no engine mutex held, so a listener is
    // free to call back into the engine.
    void notify_position(timeline::Tick tick);
    void notify_state(TransportState state, timeline::Tick tick);

    device::MidiService& m_midi_input;
    // A pointer rather than a reference: which output is selected changes, and
    // a reference cannot be reseated.
    OutputPlugin* m_output;

    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<int64_t> m_current_tick{0};
    std::atomic<bool> m_metronome_enabled{false};

    // ── Guarded by m_state_mutex ─────────────────────────────────────────────
    // Snapshot of the playing document plus transient playback state. All of
    // it is accessed from both the playback thread and command threads.
    mutable std::mutex m_state_mutex;
    std::vector<PlaybackNoteEvent> m_notes;
    std::vector<PlaybackProgramEvent> m_programs;
    std::vector<PlaybackControllerEvent> m_controllers;
    std::vector<PlaybackBendEvent> m_bends;
    std::vector<TempoSegment> m_tempo;
    std::vector<MeterSegment> m_meter;
    int64_t m_ppqn{480};
    double m_precise_tick{0.0};
    int64_t m_last_metronome_tick{-1};
    // End of the last scheduled note, so the thread knows when it has run out.
    int64_t m_content_end_tick{0};

    // Whether the output is expecting events. Replaces asking the device
    // whether a port is open: what matters now is that start() succeeded, which
    // a plugin with no port at all can also answer.
    bool m_output_started{false};

    // The slice the loop is processing: ticks [base, base + tick_span] cover
    // wall clock [base_us, base_us + span_us]. Written once per iteration and
    // read by due_us_locked.
    double  m_slice_base_tick{0.0};
    double  m_slice_tick_span{0.0};
    int64_t m_slice_base_us{0};
    double  m_slice_span_us{0.0};

    // Mixer settings per MIDI channel, and what was last transmitted for each.
    // The two are kept apart on purpose: a controller written into the score can
    // overwrite the fader's value mid-playback, and comparing against what was
    // actually sent is what stops the next unrelated edit from silently undoing
    // that.
    std::optional<ChannelMix> m_mix[16];
    // Holds what was sent, which is the scaled value, not the fader's: the
    // master moving changes what belongs on the wire without any track fader
    // moving, and comparing the unscaled values would suppress exactly that.
    std::optional<ChannelMix> m_sent_mix[16];
    uint8_t m_master_volume{127};

    struct PlayingNote {
        uint8_t channel;
        uint8_t pitch;
        int64_t end_tick;
    };
    std::vector<PlayingNote> m_playing_notes;

    // ── Recording state (guarded by m_recording_mutex) ──────────────────────
    struct PendingNote {
        uint8_t channel;
        uint8_t pitch;
        uint8_t velocity;
        int64_t start_tick;
    };
    // Key is (channel << 8) | pitch
    std::map<uint16_t, PendingNote> m_pending_notes;
    std::mutex m_recording_mutex;

    std::thread m_thread;
    std::atomic<bool> m_running{true};
    std::mutex m_callback_mutex;
    PositionCallback m_position_callback;
    NoteCommitCallback m_note_commit_callback;
    StateCallback m_state_callback;
};

} // namespace midi_composer::playback

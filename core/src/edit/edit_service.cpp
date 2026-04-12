#include "edit_service.hpp"
#include "base/logger.hpp"
#include <algorithm>
#include <cmath>

namespace midi_composer::edit {

namespace {

// ── Raw mutation helpers ─────────────────────────────────────────────────────
// These perform the actual model changes without touching revision/dirty
// state or the undo history, so they can be reused verbatim inside undo/redo
// closures. Track/note lookup is by id: the ProjectDocument address is stable
// for the document's lifetime (owned by unique_ptr), so capturing `&doc` in
// history entries is safe.

music::Track* find_track(project::ProjectDocument& doc, base::TrackId track_id) {
    auto& tracks = doc.composition().tracks();
    auto it = std::find_if(tracks.begin(), tracks.end(),
                           [track_id](const auto& t) { return t.id() == track_id; });
    return it == tracks.end() ? nullptr : &*it;
}

// ── Change recording ─────────────────────────────────────────────────────────
// Recorded from inside the apply_* helpers rather than from the public
// operations, because undo, redo and batch rollback all replay those same
// helpers — so they emit correct change lists with no extra bookkeeping.

void record_note(project::ProjectDocument& doc, project::ChangeKind kind,
                 base::TrackId track_id, const music::Note& note) {
    doc.record_change({kind, track_id, note.id, note});
}

void record_track(project::ProjectDocument& doc, project::ChangeKind kind, base::TrackId track_id) {
    doc.record_change({kind, track_id, base::NoteId{}, {}});
}

void record_global(project::ProjectDocument& doc, project::ChangeKind kind) {
    doc.record_change({kind, base::TrackId{}, base::NoteId{}, {}});
}

// Notes are kept sorted by start tick (domain invariant: event collections
// are sorted, notation and export rely on it).
void apply_insert(project::ProjectDocument& doc, base::TrackId track_id, const music::Note& note) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto& notes = track->notes();
    auto pos = std::upper_bound(notes.begin(), notes.end(), note,
        [](const music::Note& a, const music::Note& b) { return a.start < b.start; });
    notes.insert(pos, note);
    record_note(doc, project::ChangeKind::NoteCreated, track_id, note);
}

void apply_erase(project::ProjectDocument& doc, base::TrackId track_id, base::NoteId note_id) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) return;
    const music::Note removed = *it;
    notes.erase(it);
    record_note(doc, project::ChangeKind::NoteDeleted, track_id, removed);
}

// Re-sorting means a move is an erase plus an insert, so it emits
// NoteDeleted + NoteCreated rather than one NoteUpdated. Applied in order the
// net effect on the UI mirror is identical, and the note keeps its id, so
// selection survives.
void apply_move(project::ProjectDocument& doc, base::TrackId track_id, base::NoteId note_id, timeline::Tick start) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) return;
    music::Note moved = *it;
    moved.start = start;
    notes.erase(it);
    record_note(doc, project::ChangeKind::NoteDeleted, track_id, moved);
    apply_insert(doc, track_id, moved);
}

void apply_resize(project::ProjectDocument& doc, base::TrackId track_id, base::NoteId note_id, timeline::TickDuration duration) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) return;
    it->duration = duration;
    record_note(doc, project::ChangeKind::NoteUpdated, track_id, *it);
}

void apply_pitch_velocity(project::ProjectDocument& doc, base::TrackId track_id, base::NoteId note_id,
                          std::uint8_t pitch, std::uint8_t velocity) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) return;
    it->pitch = pitch;
    it->velocity = velocity;
    record_note(doc, project::ChangeKind::NoteUpdated, track_id, *it);
}

void apply_tempo(project::ProjectDocument& doc, std::uint32_t microseconds_per_quarter) {
    auto& events = doc.composition().tempo_map().events();
    if (events.empty()) {
        music::TempoEvent ev;
        ev.microseconds_per_quarter = microseconds_per_quarter;
        events.push_back(ev);
    } else {
        events.front().microseconds_per_quarter = microseconds_per_quarter;
    }
    record_global(doc, project::ChangeKind::TempoMapUpdated);
}

void apply_time_signature_map(project::ProjectDocument& doc,
                              const std::vector<music::TimeSignatureEvent>& events) {
    doc.composition().time_signature_map().events() = events;
    record_global(doc, project::ChangeKind::TimeSignatureMapUpdated);
}

// Start tick of the measure containing `tick`, walking the meter map from the
// beginning the same way the notation service segments measures.
int64_t measure_start_at(const music::Composition& comp, int64_t tick) {
    const auto& events = comp.time_signature_map().events();
    const int64_t ppqn = comp.ppqn();
    int64_t current = 0;
    while (true) {
        std::uint8_t num = 4, den = 4;
        for (const auto& e : events) {
            if (e.tick.value() <= current) { num = e.numerator; den = e.denominator; }
            else break;
        }
        // Always at least one tick, so the walk cannot stall on bad data.
        const int64_t length = std::max<int64_t>(1, ppqn * 4 * num / den);
        if (current + length > tick) return current;
        current += length;
    }
}

// The whole map is swapped rather than diffed: key changes are rare and this
// makes undo/redo trivially exact.
void apply_key_map(project::ProjectDocument& doc, const std::vector<music::KeySignatureEvent>& events) {
    doc.composition().key_signature_map().events() = events;
    record_global(doc, project::ChangeKind::KeySignatureMapUpdated);
}

void apply_name(project::ProjectDocument& doc, base::TrackId track_id, const std::string& name) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    track->set_name(name);
    record_track(doc, project::ChangeKind::TrackPropsUpdated, track_id);
}

base::Result<void> validate_note(timeline::Tick start, timeline::TickDuration duration, std::uint8_t pitch, std::uint8_t velocity) {
    if (start.value() < 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note start tick must be >= 0"});
    }
    if (duration.value() <= 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note duration must be > 0"});
    }
    if (pitch > 127) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Pitch must be between 0 and 127"});
    }
    if (velocity < 1 || velocity > 127) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Velocity must be between 1 and 127"});
    }
    return {};
}

void apply_channel(project::ProjectDocument& doc, base::TrackId track_id, std::uint8_t channel) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    track->set_midi_channel(channel);
    record_track(doc, project::ChangeKind::TrackPropsUpdated, track_id);
}

void apply_clef(project::ProjectDocument& doc, base::TrackId track_id, music::Clef clef) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    track->set_clef(clef);
    record_track(doc, project::ChangeKind::TrackPropsUpdated, track_id);
}

// Highest event id anywhere in the composition, so a newly inserted event never
// collides with ids assigned by MIDI import or by the deserializer (both use
// their own per-file counters).
std::uint64_t next_event_id(const project::ProjectDocument& doc) {
    std::uint64_t max_id = 0;
    const auto& comp = doc.composition();
    for (const auto& ev : comp.tempo_map().events())          max_id = std::max(max_id, ev.id.value());
    for (const auto& ev : comp.time_signature_map().events()) max_id = std::max(max_id, ev.id.value());
    for (const auto& track : comp.tracks()) {
        for (const auto& ev : track.controller_events()) max_id = std::max(max_id, ev.id.value());
        for (const auto& ev : track.pitch_bends())       max_id = std::max(max_id, ev.id.value());
        for (const auto& ev : track.program_changes())   max_id = std::max(max_id, ev.id.value());
    }
    return max_id + 1;
}

music::ProgramChangeEvent* find_program_at_zero(music::Track& track) {
    auto& pcs = track.program_changes();
    auto it = std::find_if(pcs.begin(), pcs.end(),
                           [](const music::ProgramChangeEvent& e) { return e.tick.value() == 0; });
    return it == pcs.end() ? nullptr : &*it;
}

// Sets the tick-0 program change to `program`, or removes it when nullopt (the
// state a track is in before an instrument was ever chosen). Tick 0 is the
// earliest possible position, so inserting at the front keeps the collection
// sorted by tick.
void apply_program_at_zero(project::ProjectDocument& doc,
                           base::TrackId track_id,
                           std::optional<std::uint8_t> program,
                           base::EventId id) {
    auto* track = find_track(doc, track_id);
    if (!track) return;
    auto* existing = find_program_at_zero(*track);

    if (!program.has_value()) {
        if (existing) {
            auto& pcs = track->program_changes();
            pcs.erase(pcs.begin() + (existing - pcs.data()));
            record_track(doc, project::ChangeKind::TrackProgramsUpdated, track_id);
        }
        return;
    }
    if (existing) {
        existing->program = *program;
    } else {
        track->program_changes().insert(track->program_changes().begin(),
                                       music::ProgramChangeEvent{id, timeline::Tick{0}, *program});
    }
    record_track(doc, project::ChangeKind::TrackProgramsUpdated, track_id);
}

void commit(project::ProjectDocument& doc, project::UndoHistory::Entry entry) {
    doc.history().push(std::move(entry));
    doc.mark_dirty();
    doc.bump_revision();
}

} // namespace

base::Result<base::NoteId> EditService::create_note(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    timeline::Tick start,
    timeline::TickDuration duration,
    std::uint8_t pitch,
    std::uint8_t velocity,
    base::NoteId forced_id)
{
    if (auto valid = validate_note(start, duration, pitch, velocity); !valid) {
        return std::unexpected(valid.error());
    }
    if (!find_track(doc, track_id)) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }

    music::Note note;
    note.id = forced_id;
    note.start = start;
    note.duration = duration;
    note.pitch = pitch;
    note.velocity = velocity;

    apply_insert(doc, track_id, note);
    commit(doc, {
        [&doc, track_id, id = note.id]() { apply_erase(doc, track_id, id); },
        [&doc, track_id, note]() { apply_insert(doc, track_id, note); },
    });
    return note.id;
}

base::Result<void> EditService::delete_note(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    base::NoteId note_id)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
    }

    const music::Note removed = *it;
    apply_erase(doc, track_id, note_id);
    commit(doc, {
        [&doc, track_id, removed]() { apply_insert(doc, track_id, removed); },
        [&doc, track_id, note_id]() { apply_erase(doc, track_id, note_id); },
    });
    return {};
}

base::Result<void> EditService::move_note(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    base::NoteId note_id,
    timeline::Tick new_start)
{
    if (new_start.value() < 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note start tick must be >= 0"});
    }
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
    }

    const timeline::Tick old_start = it->start;
    apply_move(doc, track_id, note_id, new_start);
    commit(doc, {
        [&doc, track_id, note_id, old_start]() { apply_move(doc, track_id, note_id, old_start); },
        [&doc, track_id, note_id, new_start]() { apply_move(doc, track_id, note_id, new_start); },
    });
    return {};
}

base::Result<void> EditService::resize_note(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    base::NoteId note_id,
    timeline::TickDuration new_duration)
{
    if (new_duration.value() <= 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note duration must be > 0"});
    }
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
    }

    const timeline::TickDuration old_duration = it->duration;
    apply_resize(doc, track_id, note_id, new_duration);
    commit(doc, {
        [&doc, track_id, note_id, old_duration]() { apply_resize(doc, track_id, note_id, old_duration); },
        [&doc, track_id, note_id, new_duration]() { apply_resize(doc, track_id, note_id, new_duration); },
    });
    return {};
}

base::Result<void> EditService::update_note(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    base::NoteId note_id,
    std::optional<std::uint8_t> pitch,
    std::optional<std::uint8_t> velocity)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    auto& notes = track->notes();
    auto it = std::find_if(notes.begin(), notes.end(),
                           [note_id](const auto& n) { return n.id == note_id; });
    if (it == notes.end()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
    }

    const std::uint8_t new_pitch = pitch.value_or(it->pitch);
    const std::uint8_t new_velocity = velocity.value_or(it->velocity);
    if (auto valid = validate_note(it->start, it->duration, new_pitch, new_velocity); !valid) {
        return std::unexpected(valid.error());
    }

    const std::uint8_t old_pitch = it->pitch;
    const std::uint8_t old_velocity = it->velocity;
    if (new_pitch == old_pitch && new_velocity == old_velocity) return {};

    apply_pitch_velocity(doc, track_id, note_id, new_pitch, new_velocity);
    commit(doc, {
        [&doc, track_id, note_id, old_pitch, old_velocity]() { apply_pitch_velocity(doc, track_id, note_id, old_pitch, old_velocity); },
        [&doc, track_id, note_id, new_pitch, new_velocity]() { apply_pitch_velocity(doc, track_id, note_id, new_pitch, new_velocity); },
    });
    return {};
}

base::Result<base::TrackId> EditService::create_track(
    project::ProjectDocument& doc,
    base::TrackId new_id,
    std::string name)
{
    if (name.empty()) name = "Track " + std::to_string(new_id.value());

    music::Track track(new_id, std::move(name));
    doc.composition().tracks().push_back(track);
    // Adding or removing a track shifts the whole track list (and can restore a
    // track full of notes on undo), so the UI re-snapshots instead of diffing.
    record_global(doc, project::ChangeKind::ResyncRequired);
    commit(doc, {
        [&doc, new_id]() {
            auto& tracks = doc.composition().tracks();
            auto it = std::find_if(tracks.begin(), tracks.end(),
                                   [new_id](const auto& t) { return t.id() == new_id; });
            if (it != tracks.end()) tracks.erase(it);
            record_global(doc, project::ChangeKind::ResyncRequired);
        },
        [&doc, track]() {
            doc.composition().tracks().push_back(track);
            record_global(doc, project::ChangeKind::ResyncRequired);
        },
    });
    return new_id;
}

base::Result<void> EditService::rename_track(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    std::string name)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    if (name.empty()) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Track name must not be empty"});
    }

    std::string old_name{track->name()};
    apply_name(doc, track_id, name);
    commit(doc, {
        [&doc, track_id, old_name]() { apply_name(doc, track_id, old_name); },
        [&doc, track_id, name]()     { apply_name(doc, track_id, name); },
    });
    return {};
}

base::Result<void> EditService::delete_track(
    project::ProjectDocument& doc,
    base::TrackId track_id)
{
    auto& tracks = doc.composition().tracks();
    auto it = std::find_if(tracks.begin(), tracks.end(),
                           [track_id](const auto& t) { return t.id() == track_id; });
    if (it == tracks.end()) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }

    const music::Track removed = *it;
    const auto index = static_cast<std::size_t>(std::distance(tracks.begin(), it));
    tracks.erase(it);
    record_global(doc, project::ChangeKind::ResyncRequired);
    commit(doc, {
        [&doc, removed, index]() {
            auto& ts = doc.composition().tracks();
            ts.insert(ts.begin() + static_cast<std::ptrdiff_t>(std::min(index, ts.size())), removed);
            record_global(doc, project::ChangeKind::ResyncRequired);
        },
        [&doc, track_id]() {
            auto& ts = doc.composition().tracks();
            auto i = std::find_if(ts.begin(), ts.end(),
                                  [track_id](const auto& t) { return t.id() == track_id; });
            if (i != ts.end()) ts.erase(i);
            record_global(doc, project::ChangeKind::ResyncRequired);
        },
    });
    return {};
}

base::Result<std::vector<base::NoteId>> EditService::batch_edit(
    project::ProjectDocument& doc,
    const std::vector<BatchOperation>& operations)
{
    if (operations.empty()) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Batch contains no operations"});
    }

    using Step = std::function<void()>;
    std::vector<Step> forward;   // resolved re-apply closures (for redo)
    std::vector<Step> inverse;   // rollback closures, applied in reverse
    std::vector<base::NoteId> created;

    auto rollback = [&]() {
        for (auto it = inverse.rbegin(); it != inverse.rend(); ++it) (*it)();
    };

    auto find_note = [&doc](base::TrackId track_id, base::NoteId note_id) -> music::Note* {
        auto* track = find_track(doc, track_id);
        if (!track) return nullptr;
        auto& notes = track->notes();
        auto it = std::find_if(notes.begin(), notes.end(),
                               [note_id](const auto& n) { return n.id == note_id; });
        return it == notes.end() ? nullptr : &*it;
    };

    for (const auto& op : operations) {
        base::Result<void> op_result{};
        switch (op.type) {
            case BatchOperation::Type::CreateNote: {
                const uint8_t pitch = op.pitch.value_or(60);
                const uint8_t velocity = op.velocity.value_or(100);
                if (auto valid = validate_note(op.start, op.duration, pitch, velocity); !valid) {
                    op_result = std::unexpected(valid.error());
                    break;
                }
                if (!find_track(doc, op.track_id)) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
                    break;
                }
                music::Note note;
                note.id = op.note_id;
                note.start = op.start;
                note.duration = op.duration;
                note.pitch = pitch;
                note.velocity = velocity;
                apply_insert(doc, op.track_id, note);
                created.push_back(note.id);
                forward.push_back([&doc, tid = op.track_id, note]() { apply_insert(doc, tid, note); });
                inverse.push_back([&doc, tid = op.track_id, id = note.id]() { apply_erase(doc, tid, id); });
                break;
            }
            case BatchOperation::Type::MoveNote: {
                auto* note = find_note(op.track_id, op.note_id);
                if (!note) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
                    break;
                }
                if (op.start.value() < 0) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note start tick must be >= 0"});
                    break;
                }
                const timeline::Tick old_start = note->start;
                apply_move(doc, op.track_id, op.note_id, op.start);
                forward.push_back([&doc, tid = op.track_id, nid = op.note_id, s = op.start]() { apply_move(doc, tid, nid, s); });
                inverse.push_back([&doc, tid = op.track_id, nid = op.note_id, old_start]() { apply_move(doc, tid, nid, old_start); });
                break;
            }
            case BatchOperation::Type::ResizeNote: {
                auto* note = find_note(op.track_id, op.note_id);
                if (!note) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
                    break;
                }
                if (op.duration.value() <= 0) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Note duration must be > 0"});
                    break;
                }
                const timeline::TickDuration old_duration = note->duration;
                apply_resize(doc, op.track_id, op.note_id, op.duration);
                forward.push_back([&doc, tid = op.track_id, nid = op.note_id, d = op.duration]() { apply_resize(doc, tid, nid, d); });
                inverse.push_back([&doc, tid = op.track_id, nid = op.note_id, old_duration]() { apply_resize(doc, tid, nid, old_duration); });
                break;
            }
            case BatchOperation::Type::DeleteNote: {
                auto* note = find_note(op.track_id, op.note_id);
                if (!note) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
                    break;
                }
                const music::Note removed = *note;
                apply_erase(doc, op.track_id, op.note_id);
                forward.push_back([&doc, tid = op.track_id, nid = op.note_id]() { apply_erase(doc, tid, nid); });
                inverse.push_back([&doc, tid = op.track_id, removed]() { apply_insert(doc, tid, removed); });
                break;
            }
            case BatchOperation::Type::UpdateNote: {
                auto* note = find_note(op.track_id, op.note_id);
                if (!note) {
                    op_result = std::unexpected(base::Error{base::ErrorCode::NotFound, "Note not found"});
                    break;
                }
                const uint8_t new_pitch = op.pitch.value_or(note->pitch);
                const uint8_t new_velocity = op.velocity.value_or(note->velocity);
                if (auto valid = validate_note(note->start, note->duration, new_pitch, new_velocity); !valid) {
                    op_result = std::unexpected(valid.error());
                    break;
                }
                const uint8_t old_pitch = note->pitch;
                const uint8_t old_velocity = note->velocity;
                apply_pitch_velocity(doc, op.track_id, op.note_id, new_pitch, new_velocity);
                forward.push_back([&doc, tid = op.track_id, nid = op.note_id, new_pitch, new_velocity]() {
                    apply_pitch_velocity(doc, tid, nid, new_pitch, new_velocity);
                });
                inverse.push_back([&doc, tid = op.track_id, nid = op.note_id, old_pitch, old_velocity]() {
                    apply_pitch_velocity(doc, tid, nid, old_pitch, old_velocity);
                });
                break;
            }
        }
        if (op_result) continue;   // expected<void> is truthy on success
        rollback();
        return std::unexpected(op_result.error());
    }

    commit(doc, {
        [inverse]() { for (auto it = inverse.rbegin(); it != inverse.rend(); ++it) (*it)(); },
        [forward]() { for (const auto& step : forward) step(); },
    });
    return created;
}

base::Result<void> EditService::set_track_channel(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    std::uint8_t channel)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    if (channel > 15) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "MIDI channel must be 0-15"});
    }

    const std::uint8_t old_channel = track->midi_channel();
    if (channel == old_channel) return {};

    apply_channel(doc, track_id, channel);
    commit(doc, {
        [&doc, track_id, old_channel]() { apply_channel(doc, track_id, old_channel); },
        [&doc, track_id, channel]()     { apply_channel(doc, track_id, channel); },
    });
    return {};
}

base::Result<void> EditService::set_time_signature(
    project::ProjectDocument& doc,
    timeline::Tick tick,
    std::uint8_t numerator,
    std::uint8_t denominator)
{
    if (tick.value() < 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Time signature tick must be >= 0"});
    }
    if (numerator < 1 || numerator > 32) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Time signature numerator must be 1-32"});
    }
    // Standard notation — and the MIDI meta event, which stores log2(denominator)
    // — only admits power-of-two denominators.
    if (denominator != 1 && denominator != 2 && denominator != 4 &&
        denominator != 8 && denominator != 16 && denominator != 32) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
            "Time signature denominator must be 1, 2, 4, 8, 16 or 32"});
    }

    const timeline::Tick at{measure_start_at(doc.composition(), tick.value())};

    const auto before = doc.composition().time_signature_map().events();
    auto after = before;
    auto it = std::find_if(after.begin(), after.end(),
                           [at](const auto& e) { return e.tick == at; });
    if (it != after.end()) {
        it->numerator = numerator;
        it->denominator = denominator;
    } else {
        std::uint64_t max_id = 0;
        for (const auto& e : after) max_id = std::max(max_id, e.id.value());
        after.push_back(music::TimeSignatureEvent{base::EventId{max_id + 1}, at, numerator, denominator});
        std::sort(after.begin(), after.end(),
                  [](const auto& a, const auto& b) { return a.tick < b.tick; });
    }
    if (after == before) return {};

    apply_time_signature_map(doc, after);
    commit(doc, {
        [&doc, before]() { apply_time_signature_map(doc, before); },
        [&doc, after]()  { apply_time_signature_map(doc, after); },
    });
    return {};
}

base::Result<void> EditService::set_key_signature(
    project::ProjectDocument& doc,
    timeline::Tick tick,
    std::int8_t fifths,
    bool minor)
{
    if (tick.value() < 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Key signature tick must be >= 0"});
    }
    if (fifths < -7 || fifths > 7) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Key signature must be between -7 and 7 fifths"});
    }

    const auto before = doc.composition().key_signature_map().events();
    auto after = before;

    auto it = std::find_if(after.begin(), after.end(),
                           [tick](const auto& e) { return e.tick == tick; });
    if (it != after.end()) {
        it->fifths = fifths;
        it->minor = minor;
    } else {
        std::uint64_t max_id = 0;
        for (const auto& e : after) max_id = std::max(max_id, e.id.value());
        after.push_back(music::KeySignatureEvent{base::EventId{max_id + 1}, tick, fifths, minor});
        std::sort(after.begin(), after.end(),
                  [](const auto& a, const auto& b) { return a.tick < b.tick; });
    }
    if (after == before) return {};

    apply_key_map(doc, after);
    commit(doc, {
        [&doc, before]() { apply_key_map(doc, before); },
        [&doc, after]()  { apply_key_map(doc, after); },
    });
    return {};
}

base::Result<void> EditService::set_track_clef(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    music::Clef clef)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    const music::Clef old_clef = track->clef();
    if (clef == old_clef) return {};

    apply_clef(doc, track_id, clef);
    commit(doc, {
        [&doc, track_id, old_clef]() { apply_clef(doc, track_id, old_clef); },
        [&doc, track_id, clef]()     { apply_clef(doc, track_id, clef); },
    });
    return {};
}

base::Result<void> EditService::set_track_program(
    project::ProjectDocument& doc,
    base::TrackId track_id,
    std::uint8_t program)
{
    auto* track = find_track(doc, track_id);
    if (!track) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    }
    if (program > 127) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "Program must be 0-127"});
    }

    auto* existing = find_program_at_zero(*track);
    const std::optional<std::uint8_t> old_program =
        existing ? std::optional<std::uint8_t>{existing->program} : std::nullopt;
    if (old_program == program) return {};

    // Reuse the existing event's id so undo/redo keeps addressing the same
    // event; only a fresh insertion needs a new one.
    const base::EventId id = existing ? existing->id : base::EventId{next_event_id(doc)};

    apply_program_at_zero(doc, track_id, program, id);
    commit(doc, {
        [&doc, track_id, old_program, id]() { apply_program_at_zero(doc, track_id, old_program, id); },
        [&doc, track_id, program, id]()     { apply_program_at_zero(doc, track_id, program, id); },
    });
    return {};
}

base::Result<void> EditService::set_tempo(project::ProjectDocument& doc, double bpm) {
    if (!(bpm >= 20.0 && bpm <= 400.0)) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "BPM must be between 20 and 400"});
    }
    const auto new_uspq = static_cast<std::uint32_t>(std::llround(60'000'000.0 / bpm));

    auto& events = doc.composition().tempo_map().events();
    const std::uint32_t old_uspq = events.empty() ? 500000 : events.front().microseconds_per_quarter;
    if (new_uspq == old_uspq) return {};

    apply_tempo(doc, new_uspq);
    commit(doc, {
        [&doc, old_uspq]() { apply_tempo(doc, old_uspq); },
        [&doc, new_uspq]() { apply_tempo(doc, new_uspq); },
    });
    return {};
}

} // namespace midi_composer::edit

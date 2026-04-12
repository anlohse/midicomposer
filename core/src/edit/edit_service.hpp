#pragma once

#include "base/error.hpp"
#include "base/strong_id.hpp"
#include "project/project_document.hpp"
#include "timeline/tick.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace midi_composer::edit {

// One operation of an atomic batch edit (protocol op names: "CreateNote",
// "MoveNote", "ResizeNote", "DeleteNote", "UpdateNote").
struct BatchOperation {
    enum class Type { CreateNote, MoveNote, ResizeNote, DeleteNote, UpdateNote };

    Type type{};
    base::TrackId track_id{};
    base::NoteId note_id{};                   // target for move/resize/delete/update; pre-assigned id for create
    timeline::Tick start{};                   // create: startTick, move: newStartTick
    timeline::TickDuration duration{};        // create: durationTicks, resize: newDurationTicks
    std::optional<std::uint8_t> pitch{};      // create/update
    std::optional<std::uint8_t> velocity{};   // create/update
};

// All mutations go through this service so every edit is validated and
// recorded in the document's undo history as a paired inverse operation.
class EditService final {
public:
    EditService() = default;

    base::Result<base::NoteId> create_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        timeline::Tick start,
        timeline::TickDuration duration,
        std::uint8_t pitch,
        std::uint8_t velocity,
        base::NoteId forced_id = base::NoteId{0});

    base::Result<void> delete_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id);

    base::Result<void> move_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id,
        timeline::Tick new_start);

    base::Result<void> resize_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id,
        timeline::TickDuration new_duration);

    base::Result<void> update_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id,
        std::optional<std::uint8_t> pitch,
        std::optional<std::uint8_t> velocity);

    base::Result<base::TrackId> create_track(
        project::ProjectDocument& doc,
        base::TrackId new_id,
        std::string name);

    base::Result<void> rename_track(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        std::string name);

    base::Result<void> set_track_channel(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        std::uint8_t channel);

    // Notation-only, but undoable like the other track properties.
    base::Result<void> set_track_clef(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        music::Clef clef);

    // The track's "instrument". Modelled as the program change event at tick 0
    // rather than a separate track field, so it round-trips through the native
    // format and MIDI import/export with no extra plumbing, and shows up in the
    // MIDI events panel like any other event. One is inserted if the track has
    // none; later program changes in the same track are left alone.
    base::Result<void> set_track_program(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        std::uint8_t program);

    base::Result<void> delete_track(
        project::ProjectDocument& doc,
        base::TrackId track_id);

    base::Result<void> set_tempo(
        project::ProjectDocument& doc,
        double bpm);

    // Inserts or replaces the time signature at the start of the measure
    // containing `tick`. A meter change moves every following bar line, so it
    // must land on a measure boundary or the notation stops making sense.
    base::Result<void> set_time_signature(
        project::ProjectDocument& doc,
        timeline::Tick tick,
        std::uint8_t numerator,
        std::uint8_t denominator);

    // Inserts or replaces the key signature at `tick`. Undo restores the whole
    // previous map, which is always correct regardless of where it landed.
    base::Result<void> set_key_signature(
        project::ProjectDocument& doc,
        timeline::Tick tick,
        std::int8_t fifths,
        bool minor);

    // Applies all operations atomically: if any fails, the already-applied
    // ones are rolled back and nothing is recorded. On success the whole
    // batch is a single undo entry and a single revision bump. Returns the
    // ids of created notes, in operation order.
    base::Result<std::vector<base::NoteId>> batch_edit(
        project::ProjectDocument& doc,
        const std::vector<BatchOperation>& operations);
};

} // namespace midi_composer::edit

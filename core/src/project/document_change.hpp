#pragma once

#include "base/strong_id.hpp"
#include "music/note.hpp"
#include <vector>

namespace midi_composer::project {

// One incremental change to a document, recorded by the mutation helpers as
// they run. Because undo/redo and batch rollback replay those same helpers,
// they produce correct change lists without any extra bookkeeping.
//
// Payloads stay deliberately thin: anything the UI needs beyond a note is read
// back off the live document when the patch is serialized, which happens
// immediately after the mutation commits.
enum class ChangeKind {
    NoteCreated,
    NoteUpdated,
    NoteDeleted,
    TrackPropsUpdated,       // name, channel, volume, pan, mute, solo, arm
    MasterVolumeUpdated,     // the master fader, which belongs to no track
    TrackProgramsUpdated,    // the track's program-change list
    // Whole-list replacements, like the programs above. These lists are small
    // and an edit can reorder them, so shipping the list costs less than
    // describing the delta and leaves nothing to get out of order.
    TrackControllersUpdated,
    TrackPitchBendsUpdated,
    TempoMapUpdated,
    TimeSignatureMapUpdated,
    KeySignatureMapUpdated,
    // Structural change (track added/removed, document reloaded) where sending
    // a diff would cost more than it saves. The UI re-snapshots the document.
    ResyncRequired,
};

struct DocumentChange {
    ChangeKind kind{};
    base::TrackId track_id{};
    base::NoteId note_id{};
    music::Note note{};      // NoteCreated / NoteUpdated
};

using DocumentChangeList = std::vector<DocumentChange>;

} // namespace midi_composer::project

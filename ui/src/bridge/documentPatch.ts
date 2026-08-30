import { DocumentSnapshot, NoteSnapshot, TrackSnapshot, ProgramChangeEventSnapshot,
         ControllerEventSnapshot, PitchBendEventSnapshot,
         TempoSnapshot, TimeSignatureSnapshot, KeySignatureSnapshot } from '../models/document';

// Incremental changes pushed by the core after every committed mutation, so the
// UI can update its mirror instead of re-fetching whole snapshots.
// See core/src/project/document_change.hpp for the producing side.

export type DocumentChange =
    | { kind: 'noteCreated';           trackId: number; note: NoteSnapshot }
    | { kind: 'noteUpdated';           trackId: number; note: NoteSnapshot }
    | { kind: 'noteDeleted';           trackId: number; noteId: number }
    | { kind: 'masterVolumeUpdated';   masterVolume: number }
    | { kind: 'trackPropsUpdated';     track: TrackPropsPatch }
    | { kind: 'trackProgramsUpdated';  trackId: number; programChanges: ProgramChangeEventSnapshot[] }
    // Whole-list replacements: an edit can move an event in tick order, and these
    // lists are small enough that sending them costs less than describing a delta.
    | { kind: 'trackControllersUpdated'; trackId: number; controllerEvents: ControllerEventSnapshot[] }
    | { kind: 'trackPitchBendsUpdated';  trackId: number; pitchBends: PitchBendEventSnapshot[] }
    | { kind: 'tempoMapUpdated';       tempoMap: TempoSnapshot[] }
    | { kind: 'timeSignatureMapUpdated'; timeSignatureMap: TimeSignatureSnapshot[] }
    | { kind: 'keySignatureMapUpdated';  keySignatureMap: KeySignatureSnapshot[] };

// Scalar track fields only — the core never ships note/event collections here.
export type TrackPropsPatch = Pick<TrackSnapshot,
    'id' | 'name' | 'midiChannel' | 'volume' | 'pan' | 'muted' | 'solo' | 'armed'>;

export interface DocumentPatch {
    documentId: number;
    baseRevision: number;
    revision: number;
    dirty: boolean;
    canUndo: boolean;
    canRedo: boolean;
    /** Structural change: discard the mirror and re-snapshot instead of patching. */
    resync: boolean;
    changes: DocumentChange[];
}

// Ids cross the bridge as numbers but the snapshot types declare them as
// strings, and runtime values have historically been numbers. Compare loosely.
function sameId(a: unknown, b: unknown): boolean {
    return String(a) === String(b);
}

function replaceTrack(doc: DocumentSnapshot, trackId: number,
                      update: (track: TrackSnapshot) => TrackSnapshot): DocumentSnapshot | null {
    const index = doc.tracks.findIndex(t => sameId(t.id, trackId));
    if (index < 0) return null;                     // unknown track → resync
    const tracks = doc.tracks.slice();
    tracks[index] = update(tracks[index]);
    return { ...doc, tracks };
}

// Notes stay sorted by start tick, matching the core's invariant, so notation
// and rendering see the same order a fresh snapshot would give.
function insertSorted(notes: NoteSnapshot[], note: NoteSnapshot): NoteSnapshot[] {
    const out = notes.slice();
    let i = out.length;
    while (i > 0 && out[i - 1].startTick > note.startTick) i--;
    out.splice(i, 0, note);
    return out;
}

/**
 * Applies one change to a document mirror, returning a new object (Lit needs a
 * fresh identity to re-render) or `null` when the change cannot be applied and
 * the caller should re-snapshot.
 */
function applyChange(doc: DocumentSnapshot, change: DocumentChange): DocumentSnapshot | null {
    switch (change.kind) {
        case 'masterVolumeUpdated':
            return { ...doc, masterVolume: change.masterVolume };

        case 'noteCreated':
            return replaceTrack(doc, change.trackId, t => ({
                ...t, notes: insertSorted(t.notes.filter(n => !sameId(n.id, change.note.id)), change.note),
            }));

        case 'noteUpdated':
            return replaceTrack(doc, change.trackId, t => ({
                ...t, notes: t.notes.map(n => sameId(n.id, change.note.id) ? change.note : n),
            }));

        case 'noteDeleted':
            return replaceTrack(doc, change.trackId, t => ({
                ...t, notes: t.notes.filter(n => !sameId(n.id, change.noteId)),
            }));

        case 'trackPropsUpdated':
            return replaceTrack(doc, Number(change.track.id), t => ({ ...t, ...change.track, id: t.id }));

        case 'trackProgramsUpdated':
            return replaceTrack(doc, change.trackId, t => ({ ...t, programChanges: change.programChanges }));

        case 'trackControllersUpdated':
            return replaceTrack(doc, change.trackId, t => ({ ...t, controllerEvents: change.controllerEvents }));

        case 'trackPitchBendsUpdated':
            return replaceTrack(doc, change.trackId, t => ({ ...t, pitchBends: change.pitchBends }));

        case 'tempoMapUpdated':
            return { ...doc, tempoMap: change.tempoMap };

        case 'timeSignatureMapUpdated':
            return { ...doc, timeSignatureMap: change.timeSignatureMap };

        case 'keySignatureMapUpdated':
            return { ...doc, keySignatureMap: change.keySignatureMap };

        default:
            // Unknown change kind (core newer than UI): resync rather than
            // silently drifting out of sync.
            return null;
    }
}

/**
 * Applies a whole patch to a mirror. Returns the new document, or `null` if the
 * caller must re-snapshot — which happens when the patch is flagged `resync`,
 * when it does not build on the mirror's current revision (a lost or reordered
 * notification), or when any individual change could not be applied.
 */
export function applyDocumentPatch(doc: DocumentSnapshot, patch: DocumentPatch): DocumentSnapshot | null {
    if (patch.resync) return null;
    if (patch.baseRevision !== doc.revision) return null;

    let next: DocumentSnapshot | null = doc;
    for (const change of patch.changes) {
        next = applyChange(next!, change);
        if (!next) return null;
    }
    return {
        ...next!,
        revision: patch.revision,
        dirty: patch.dirty,
        canUndo: patch.canUndo,
        canRedo: patch.canRedo,
    };
}

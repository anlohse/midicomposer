import { test } from 'node:test';
import assert from 'node:assert/strict';
import { applyDocumentPatch, DocumentPatch } from '../src/bridge/documentPatch';
import { DocumentSnapshot } from '../src/models/document';

function mirror(revision = 5): DocumentSnapshot {
    return {
        id: 1, title: 'test', ppqn: 480, masterVolume: 127, revision, dirty: false,
        filePath: '', canUndo: false, canRedo: false,
        tempoMap: [{ tick: 0, bpm: 120 }],
        timeSignatureMap: [{ tick: 0, numerator: 4, denominator: 4 }],
        keySignatureMap: [{ tick: 0, fifths: 0, minor: false }],
        tracks: [{
            id: '7', name: 'Piano', midiChannel: 0, clef: 'treble', volume: 100, pan: 64,
            muted: false, solo: false, armed: false,
            notes: [
                { id: '1', startTick: 0, durationTicks: 480, pitch: 60, velocity: 100 },
                { id: '2', startTick: 960, durationTicks: 480, pitch: 64, velocity: 100 },
            ],
            controllerEvents: [], pitchBends: [], programChanges: [],
        }],
    };
}

function patch(changes: any[], over: Partial<DocumentPatch> = {}): DocumentPatch {
    return {
        documentId: 1, baseRevision: 5, revision: 6,
        dirty: true, canUndo: true, canRedo: false, resync: false, changes,
        ...over,
    };
}

test('a created note is inserted in start-tick order', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'noteCreated', trackId: 7,
        note: { id: '3', startTick: 480, durationTicks: 480, pitch: 62, velocity: 100 },
    }]));
    assert.ok(next);
    assert.deepEqual(next!.tracks[0].notes.map(n => n.startTick), [0, 480, 960]);
});

test('a created note appends after notes at the same tick', () => {
    // Matches the core's upper_bound insertion, so chords keep a stable order.
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'noteCreated', trackId: 7,
        note: { id: '3', startTick: 0, durationTicks: 480, pitch: 67, velocity: 100 },
    }]));
    assert.deepEqual(next!.tracks[0].notes.map(n => n.id), ['1', '3', '2']);
});

test('an updated note replaces in place', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'noteUpdated', trackId: 7,
        note: { id: '2', startTick: 960, durationTicks: 240, pitch: 65, velocity: 42 },
    }]));
    assert.equal(next!.tracks[0].notes.length, 2);
    assert.equal(next!.tracks[0].notes[1].pitch, 65);
    assert.equal(next!.tracks[0].notes[1].velocity, 42);
});

test('a deleted note is removed', () => {
    const next = applyDocumentPatch(mirror(), patch([
        { kind: 'noteDeleted', trackId: 7, noteId: 1 },
    ]));
    assert.deepEqual(next!.tracks[0].notes.map(n => n.id), ['2']);
});

test('a move arrives as delete then create and re-sorts', () => {
    // The core emits this pair because re-sorting is an erase plus an insert.
    const next = applyDocumentPatch(mirror(), patch([
        { kind: 'noteDeleted', trackId: 7, noteId: 1 },
        { kind: 'noteCreated', trackId: 7,
          note: { id: '1', startTick: 1440, durationTicks: 480, pitch: 60, velocity: 100 } },
    ]));
    assert.deepEqual(next!.tracks[0].notes.map(n => [n.id, n.startTick]),
                     [['2', 960], ['1', 1440]]);
});

test('track properties are merged without losing the note list', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'trackPropsUpdated',
        track: { id: '7', name: 'Bass', midiChannel: 3, clef: 'bass',
                 volume: 80, pan: 20, muted: true, solo: false, armed: false },
    }]));
    assert.equal(next!.tracks[0].name, 'Bass');
    assert.equal(next!.tracks[0].clef, 'bass');
    assert.equal(next!.tracks[0].midiChannel, 3);
    assert.equal(next!.tracks[0].notes.length, 2, 'notes must survive a props patch');
});

test('the event maps are replaced wholesale', () => {
    const next = applyDocumentPatch(mirror(), patch([
        { kind: 'tempoMapUpdated', tempoMap: [{ tick: 0, bpm: 96 }] },
        { kind: 'keySignatureMapUpdated', keySignatureMap: [{ tick: 0, fifths: 2, minor: false }] },
        { kind: 'timeSignatureMapUpdated',
          timeSignatureMap: [{ tick: 0, numerator: 6, denominator: 8 }] },
    ]));
    assert.equal(next!.tempoMap[0].bpm, 96);
    assert.equal(next!.keySignatureMap[0].fifths, 2);
    assert.equal(next!.timeSignatureMap[0].numerator, 6);
});

test('document-level flags come from the patch', () => {
    const next = applyDocumentPatch(mirror(), patch([], { revision: 9, canUndo: true, canRedo: true }));
    assert.equal(next!.revision, 9);
    assert.equal(next!.dirty, true);
    assert.equal(next!.canUndo, true);
    assert.equal(next!.canRedo, true);
});

test('the mirror is never mutated in place', () => {
    // Lit re-renders on identity change, so every applied patch must return new
    // objects for whatever it touched.
    const before = mirror();
    const snapshot = JSON.stringify(before);
    const next = applyDocumentPatch(before, patch([
        { kind: 'noteDeleted', trackId: 7, noteId: 1 },
    ]));
    assert.equal(JSON.stringify(before), snapshot, 'input must be untouched');
    assert.notEqual(next!.tracks, before.tracks);
    assert.notEqual(next!.tracks[0].notes, before.tracks[0].notes);
});

// ─── Resync paths ────────────────────────────────────────────────────────────

test('a resync patch asks the caller to re-snapshot', () => {
    assert.equal(applyDocumentPatch(mirror(), patch([], { resync: true })), null);
});

test('a revision gap asks the caller to re-snapshot', () => {
    // A lost or reordered notification must self-heal rather than drift.
    assert.equal(applyDocumentPatch(mirror(), patch([], { baseRevision: 4 })), null);
    assert.equal(applyDocumentPatch(mirror(), patch([], { baseRevision: 6 })), null);
});

test('an unknown change kind asks the caller to re-snapshot', () => {
    // A core newer than the UI must not silently drop changes.
    assert.equal(applyDocumentPatch(mirror(), patch([{ kind: 'somethingNew' } as any])), null);
});

test('a change naming an unknown track asks the caller to re-snapshot', () => {
    assert.equal(applyDocumentPatch(mirror(), patch([
        { kind: 'noteDeleted', trackId: 999, noteId: 1 },
    ])), null);
});

// ─── Controller events and pitch bends ───────────────────────────────────────
//
// Both arrive as whole-list replacements, so the mirror takes the list as given
// rather than merging: the core has already sorted it and dropped what was
// deleted.

test('a controller list replaces whatever the mirror had', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'trackControllersUpdated', trackId: 7,
        controllerEvents: [
            { id: '10', tick: 0, controller: 7, value: 100 },
            { id: '11', tick: 480, controller: 10, value: 64 },
        ],
    }]));
    assert.ok(next);
    assert.deepEqual(next!.tracks[0].controllerEvents.map(e => e.id), ['10', '11']);
    assert.equal(next!.tracks[0].controllerEvents[1].controller, 10);
    // Nothing else on the track is disturbed.
    assert.equal(next!.tracks[0].notes.length, 2);
    assert.deepEqual(next!.tracks[0].pitchBends, []);
});

test('an emptied controller list is a deletion', () => {
    const before = mirror();
    before.tracks[0].controllerEvents = [{ id: '10', tick: 0, controller: 7, value: 100 }];
    const next = applyDocumentPatch(before, patch([{
        kind: 'trackControllersUpdated', trackId: 7, controllerEvents: [],
    }]));
    assert.deepEqual(next!.tracks[0].controllerEvents, []);
});

test('a pitch bend list replaces and keeps signed values', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'trackPitchBendsUpdated', trackId: 7,
        pitchBends: [{ id: '20', tick: 0, value: -8192 }, { id: '21', tick: 240, value: 8191 }],
    }]));
    assert.ok(next);
    assert.deepEqual(next!.tracks[0].pitchBends.map(e => e.value), [-8192, 8191]);
});

test('an unknown track in a controller patch forces a resync', () => {
    const next = applyDocumentPatch(mirror(), patch([{
        kind: 'trackControllersUpdated', trackId: 999, controllerEvents: [],
    }]));
    assert.equal(next, null);
});

test('a master volume change patches without touching the tracks', () => {
    const before = mirror();
    const next = applyDocumentPatch(before, patch([
        { kind: 'masterVolumeUpdated', masterVolume: 64 },
    ]));
    assert.ok(next);
    assert.equal(next!.masterVolume, 64);
    // The master belongs to no track, so nothing about them may move — the
    // track faders in particular keep their own values.
    assert.deepEqual(next!.tracks, before.tracks);
});

test('a master volume change does not force a resync', () => {
    // It arrives once per pixel of a drag; falling back to a full re-snapshot
    // would fetch the whole document that often.
    const next = applyDocumentPatch(mirror(), patch([
        { kind: 'masterVolumeUpdated', masterVolume: 0 },
    ]));
    assert.ok(next);
    assert.equal(next!.masterVolume, 0);
});

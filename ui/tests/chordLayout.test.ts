import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chordStemDown, layoutChord, groupChords } from '../src/services/chordLayout';
import { NoteFragment } from '../src/services/notationService';

// Treble: the middle line (B4) is step 41. Every case below is stated in steps
// relative to that, so the numbers mean something to read.
const MIDDLE = 41;

function fragment(over: Partial<NoteFragment>): NoteFragment {
    return {
        noteId: '1', pitch: 60, startTick: 0, durationTicks: 480,
        tieStart: false, tieEnd: false, noteValue: 'quarter', dotted: false,
        triplet: null, step: 35, alteration: 0, accidental: null,
        ...over,
    } as NoteFragment;
}

// ── Which way the stem points ────────────────────────────────────────────────

test('a chord below the middle line stems up', () => {
    assert.equal(chordStemDown([MIDDLE - 6, MIDDLE - 4, MIDDLE - 2], MIDDLE), false);
});

test('a chord above the middle line stems down', () => {
    assert.equal(chordStemDown([MIDDLE + 2, MIDDLE + 4, MIDDLE + 6], MIDDLE), true);
});

test('the note furthest from the middle decides, not the majority', () => {
    // Two notes just below the middle and one far above it. Counting heads
    // would stem up and run the stem off the top of the staff.
    assert.equal(chordStemDown([MIDDLE - 1, MIDDLE - 2, MIDDLE + 8], MIDDLE), true);
});

test('a chord balanced around the middle stems down', () => {
    // The convention, and it keeps the stem clear of anything above the staff.
    assert.equal(chordStemDown([MIDDLE - 4, MIDDLE + 4], MIDDLE), true);
});

// ── Seconds ──────────────────────────────────────────────────────────────────

test('notes a third apart both sit on the same side', () => {
    const { placements } = layoutChord([MIDDLE - 6, MIDDLE - 4, MIDDLE - 2], MIDDLE);
    assert.deepEqual(placements.map(p => p.displaced), [false, false, false]);
});

test('a second displaces one of its two notes', () => {
    const steps = [MIDDLE - 6, MIDDLE - 5];
    const { stemDown, placements } = layoutChord(steps, MIDDLE);
    assert.equal(stemDown, false);
    // Stem up reads from the bottom, so the lower note holds its place and the
    // upper one crosses.
    assert.deepEqual(placements.map(p => p.displaced), [false, true]);
});

test('a second under a downward stem displaces the other one', () => {
    const steps = [MIDDLE + 5, MIDDLE + 6];
    const { stemDown, placements } = layoutChord(steps, MIDDLE);
    assert.equal(stemDown, true);
    // Reading from the top now, so the upper note is the one that stays.
    assert.deepEqual(placements.map(p => p.displaced), [true, false]);
});

test('a cluster alternates instead of pushing everything across', () => {
    // Three notes a step apart. The middle one moves; displacing all three
    // would draw a straight column on the wrong side of the stem.
    const { placements } = layoutChord([MIDDLE - 6, MIDDLE - 5, MIDDLE - 4], MIDDLE);
    assert.deepEqual(placements.map(p => p.displaced), [false, true, false]);
});

test('placements come back in the order the notes were given', () => {
    // The caller zips these onto its own fragments, so the sorting inside must
    // not leak out.
    const { placements } = layoutChord([MIDDLE - 4, MIDDLE - 6, MIDDLE - 5], MIDDLE);
    assert.deepEqual(placements.map(p => p.step), [MIDDLE - 4, MIDDLE - 6, MIDDLE - 5]);
    assert.deepEqual(placements.map(p => p.displaced), [false, false, true]);
});

test('a single note is never displaced and spans nothing', () => {
    const { placements, lowestStep, highestStep } = layoutChord([MIDDLE - 3], MIDDLE);
    assert.deepEqual(placements.map(p => p.displaced), [false]);
    assert.equal(lowestStep, MIDDLE - 3);
    assert.equal(highestStep, MIDDLE - 3);
});

test('the span is the outermost pair, for the stem to reach', () => {
    const { lowestStep, highestStep } = layoutChord([MIDDLE, MIDDLE - 7, MIDDLE + 3], MIDDLE);
    assert.equal(lowestStep, MIDDLE - 7);
    assert.equal(highestStep, MIDDLE + 3);
});

// ── Grouping ─────────────────────────────────────────────────────────────────

test('notes starting together with the same value are one chord', () => {
    const groups = groupChords([
        fragment({ noteId: 'a', step: 35 }),
        fragment({ noteId: 'b', step: 37 }),
        fragment({ noteId: 'c', step: 39 }),
    ]);
    assert.equal(groups.length, 1);
    assert.deepEqual(groups[0].map(f => f.noteId), ['a', 'b', 'c']);
});

test('notes starting together with different values stay apart', () => {
    // They cannot share a stem without the stem stating a duration only one of
    // them has; real engraving would put them in separate voices.
    const groups = groupChords([
        fragment({ noteId: 'a', noteValue: 'quarter' }),
        fragment({ noteId: 'b', noteValue: 'half' }),
    ]);
    assert.equal(groups.length, 2);
});

test('a dotted note is not the same value as an undotted one', () => {
    const groups = groupChords([
        fragment({ noteId: 'a', dotted: false }),
        fragment({ noteId: 'b', dotted: true }),
    ]);
    assert.equal(groups.length, 2);
});

test('a triplet member does not join a plain note of the same value', () => {
    const groups = groupChords([
        fragment({ noteId: 'a', triplet: null }),
        fragment({ noteId: 'b', triplet: { startTick: 0, durationTicks: 960 } }),
    ]);
    assert.equal(groups.length, 2);
});

test('notes at different ticks are different chords', () => {
    const groups = groupChords([
        fragment({ noteId: 'a', startTick: 0 }),
        fragment({ noteId: 'b', startTick: 480 }),
    ]);
    assert.equal(groups.length, 2);
});

test('grouping keeps the order the fragments arrived in', () => {
    // Anything downstream that walks a measure in time still can.
    const groups = groupChords([
        fragment({ noteId: 'a', startTick: 0 }),
        fragment({ noteId: 'b', startTick: 480 }),
        fragment({ noteId: 'c', startTick: 0 }),
    ]);
    assert.deepEqual(groups.map(g => g[0].startTick), [0, 480]);
    assert.deepEqual(groups[0].map(f => f.noteId), ['a', 'c']);
});

test('a forced direction decides which note of a second moves', () => {
    const steps = [MIDDLE - 6, MIDDLE - 5];
    // Left to itself this chord stems up and moves the upper note. Under a beam
    // that points down, the lower one moves instead.
    assert.deepEqual(layoutChord(steps, MIDDLE).placements.map(p => p.displaced),
                     [false, true]);
    assert.deepEqual(layoutChord(steps, MIDDLE, true).placements.map(p => p.displaced),
                     [true, false]);
});

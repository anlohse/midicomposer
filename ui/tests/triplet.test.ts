import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NotationService } from '../src/services/notationService';
import { DocumentSnapshot, NoteSnapshot } from '../src/models/document';
import { durationTicks, quantizeSpan, snapTicks, snapToGrid } from '../src/models/snap';

const PPQN = 480;
const QUARTER = PPQN;

// Three in the time of two, at ppqn 480.
const T_HALF      = 640;   // three fill a whole note
const T_QUARTER   = 320;   // three fill a half
const T_EIGHTH    = 160;   // three fill a quarter
const T_SIXTEENTH = 80;    // three fill an eighth

interface Options {
    ppqn?: number;
    timeSignatureMap?: Array<{ tick: number; numerator: number; denominator: number }>;
}

function doc(notes: Array<[number, number, number]>, opts: Options = {}): DocumentSnapshot {
    return {
        id: 1, title: 'test', ppqn: opts.ppqn ?? PPQN, revision: 0, dirty: false,
        filePath: '', canUndo: false, canRedo: false,
        tempoMap: [{ tick: 0, bpm: 120 }],
        timeSignatureMap: opts.timeSignatureMap ?? [{ tick: 0, numerator: 4, denominator: 4 }],
        keySignatureMap: [{ tick: 0, fifths: 0, minor: false }],
        tracks: [{
            id: '1', name: 'T', midiChannel: 0, clef: 'treble', volume: 100, pan: 64,
            muted: false, solo: false, armed: false,
            notes: notes.map(([startTick, pitch, dur], i): NoteSnapshot => ({
                id: String(i + 1), startTick, pitch, durationTicks: dur, velocity: 100,
            })),
            controllerEvents: [], pitchBends: [], programChanges: [],
        }],
    };
}

const bars = (d: DocumentSnapshot, n = 2) => NotationService.getMeasures(d, 0, n);
/** Fragment shape: value, offset in the bar, and the group it was bracketed into. */
const notesOf = (m: any) => m.fragments.map((f: any) =>
    `${f.noteValue}@${f.startTick - m.startTick}` +
    (f.triplet ? `[3:${f.triplet.startTick}+${f.triplet.durationTicks}]` : ''));
const restsOf = (m: any) => m.rests.map((r: any) =>
    `${r.restValue}@${r.startTick - m.startTick}` + (r.triplet ? '[3]' : ''));

// ─── Notation ────────────────────────────────────────────────────────────────

test('three eighth triplets fill one beat and share a group', () => {
    const [bar] = bars(doc([
        [0, 60, T_EIGHTH], [T_EIGHTH, 62, T_EIGHTH], [T_EIGHTH * 2, 64, T_EIGHTH],
    ]));
    assert.deepEqual(notesOf(bar), [
        'eighth@0[3:0+480]', 'eighth@160[3:0+480]', 'eighth@320[3:0+480]',
    ]);
    // The three fill the beat exactly, so there is no rest inside the group,
    // and the remaining three beats are written as plain rests.
    assert.deepEqual(restsOf(bar), ['quarter@480', 'half@960']);
});

test('each triplet value is drawn as the value it divides', () => {
    for (const [ticks, expected] of [
        [T_HALF, 'half'], [T_QUARTER, 'quarter'],
        [T_EIGHTH, 'eighth'], [T_SIXTEENTH, 'sixteenth'],
    ] as Array<[number, string]>) {
        const [bar] = bars(doc([[0, 60, ticks]]));
        assert.equal(bar.fragments[0].noteValue, expected, `${ticks} ticks`);
        assert.equal(bar.fragments[0].triplet?.durationTicks, ticks * 3, `${ticks} span`);
    }
});

test('a triplet group starts where such a group can start', () => {
    // Second beat: an eighth-triplet group spans one quarter, so tick 480 is the
    // head of the second group, not an offset inside the first.
    const [bar] = bars(doc([[QUARTER, 60, T_EIGHTH]]));
    assert.deepEqual(notesOf(bar), ['eighth@480[3:480+480]']);
});

test('a plain value on a triplet position is not a triplet', () => {
    // The position is fine; the length is not. Both have to hold.
    const [bar] = bars(doc([[0, 60, QUARTER], [QUARTER, 62, 240]]));
    assert.deepEqual(notesOf(bar), ['quarter@0', 'eighth@480']);
});

test('a triplet length off the triplet grid is not bracketed', () => {
    // 160 ticks starting at 240 falls between the slots of every group, so it is
    // imported material rather than a triplet, and gets no bracket.
    const [bar] = bars(doc([[240, 60, T_EIGHTH]]));
    assert.equal(bar.fragments[0].triplet, null);
});

test('a gap inside a triplet group becomes a triplet rest', () => {
    // Two of the three sound; the third is silent and must keep its true length,
    // or the bar no longer adds up.
    const [bar] = bars(doc([[0, 60, T_EIGHTH], [T_EIGHTH, 62, T_EIGHTH]]));
    assert.deepEqual(notesOf(bar), ['eighth@0[3:0+480]', 'eighth@160[3:0+480]']);
    assert.deepEqual(restsOf(bar), ['eighth@320[3]', 'quarter@480', 'half@960']);
});

test('a recorded triplet snaps to the exact value, a near miss does not', () => {
    // Release slack on a triplet eighth (ideal 160).
    assert.equal(NotationService.snapDuration(150, PPQN), T_EIGHTH);
    // 600 is a quarter plus a sixteenth. It is within a 32nd of a triplet half
    // (640) but that is a much louder claim to make, so it is left alone.
    assert.equal(NotationService.snapDuration(600, PPQN), 600);
});

test('triplets survive a ppqn that does not divide by three', () => {
    // At ppqn 128 a triplet eighth is 64/3 ticks, so no member is a whole number
    // and the three cannot each be the same length.
    const ppqn = 128;
    const each = Math.round(ppqn / 3);
    const [bar] = bars(doc([
        [0, 60, each], [each, 62, each], [each * 2, 64, ppqn - each * 2],
    ], { ppqn }));
    assert.deepEqual(bar.fragments.map((f: any) => f.noteValue),
                     ['eighth', 'eighth', 'eighth']);
    assert.ok(bar.fragments.every((f: any) => f.triplet?.startTick === 0));
});

test('a triplet group in a compound meter is measured from the bar line', () => {
    // 6/8: the bar is 3 quarters. An eighth-triplet group still spans a quarter,
    // and the second group starts at 480, not at some offset the meter shifted.
    const [bar] = bars(doc([[QUARTER, 60, T_EIGHTH]], {
        timeSignatureMap: [{ tick: 0, numerator: 6, denominator: 8 }],
    }));
    assert.equal(bar.fragments[0].triplet?.startTick, QUARTER);
});

test('a triplet in a later bar is grouped against its own bar line', () => {
    // 7/8 bars are 1680 ticks, which is not a multiple of the 480-tick group, so
    // grouping from tick 0 instead of the bar line would put the slots off.
    const measures = bars(doc([[1680, 60, T_EIGHTH]], {
        timeSignatureMap: [{ tick: 0, numerator: 7, denominator: 8 }],
    }));
    assert.equal(measures[1].fragments[0].triplet?.startTick, 1680);
});

// ─── Snapping ────────────────────────────────────────────────────────────────

test('triplet mode makes each grid two thirds of its plain step', () => {
    assert.equal(snapTicks(true, '1/2',  'quarter', PPQN, true), T_HALF);
    assert.equal(snapTicks(true, '1/4',  'quarter', PPQN, true), T_QUARTER);
    assert.equal(snapTicks(true, '1/8',  'quarter', PPQN, true), T_EIGHTH);
    assert.equal(snapTicks(true, '1/16', 'quarter', PPQN, true), T_SIXTEENTH);
});

test('auto follows the value being inserted, triplets included', () => {
    assert.equal(snapTicks(true, 'auto', 'eighth', PPQN, true), T_EIGHTH);
    assert.equal(durationTicks('eighth', PPQN, true), T_EIGHTH);
    assert.equal(durationTicks('half', PPQN, true), T_HALF);
});

test('the toggle is off by default and off means plain', () => {
    assert.equal(snapTicks(true, '1/8', 'quarter', PPQN), 240);
    assert.equal(durationTicks('eighth', PPQN), 240);
});

test('triplets still snap off entirely when snapping is off', () => {
    assert.equal(snapTicks(false, '1/8', 'quarter', PPQN, true), 0);
});

test('the three positions of a group land exactly on the beat', () => {
    const step = snapTicks(true, '1/8', 'quarter', PPQN, true);
    assert.deepEqual([0, 1, 2, 3].map(i => snapToGrid(i * step, step)),
                     [0, 160, 320, 480]);
});

test('a fractional grid does not drift over a piece', () => {
    // ppqn 100: a triplet eighth is 33.33 ticks. Rounding the step to 33 would
    // put the 30th position 10 ticks early and the error would keep growing;
    // rounding the position instead keeps every one within half a tick.
    const step = snapTicks(true, '1/8', 'quarter', 100, true);
    for (const i of [3, 30, 300]) {
        const exact = i * 100 / 3;
        assert.ok(Math.abs(snapToGrid(exact, step) - exact) <= 0.5,
                  `position ${i} drifted`);
    }
    // Every third position is a whole beat, exactly.
    assert.equal(snapToGrid(3 * step, step), 100);
    assert.equal(snapToGrid(300 * step, step), 10_000);
});

test('a grid position is always a whole tick', () => {
    const step = snapTicks(true, '1/8', 'quarter', 100, true);
    for (let i = 0; i < 12; i++) {
        assert.equal(snapToGrid(i * step + 3, step) % 1, 0, `position ${i}`);
    }
});

test('quantizeSpan moves a length by whole steps, wherever it started', () => {
    const step = snapTicks(true, '1/8', 'quarter', PPQN, true);
    assert.equal(quantizeSpan(T_EIGHTH * 2, step), 320);
    assert.equal(quantizeSpan(T_EIGHTH * 2 + 20, step), 320);
    assert.equal(quantizeSpan(-T_EIGHTH, step), -160);
    // Off: whole ticks, nothing else.
    assert.equal(quantizeSpan(157.4, 0), 157);
});

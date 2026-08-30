import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NotationService } from '../src/services/notationService';
import { DocumentSnapshot, NoteSnapshot } from '../src/models/document';

const PPQN = 480;
const QUARTER = PPQN;
const BAR_4_4 = PPQN * 4;

interface Options {
    ppqn?: number;
    timeSignatureMap?: Array<{ tick: number; numerator: number; denominator: number }>;
    keySignatureMap?: Array<{ tick: number; fifths: number; minor: boolean }>;
}

/** Minimal single-track document; only the fields the notation service reads. */
function doc(notes: Array<[number, number, number]>, opts: Options = {}): DocumentSnapshot {
    return {
        id: 1, title: 'test', ppqn: opts.ppqn ?? PPQN, masterVolume: 127, revision: 0, dirty: false,
        filePath: '', canUndo: false, canRedo: false,
        tempoMap: [{ tick: 0, bpm: 120 }],
        timeSignatureMap: opts.timeSignatureMap ?? [{ tick: 0, numerator: 4, denominator: 4 }],
        keySignatureMap: opts.keySignatureMap ?? [{ tick: 0, fifths: 0, minor: false }],
        tracks: [{
            id: '1', name: 'T', midiChannel: 0, clef: 'treble', volume: 100, pan: 64,
            muted: false, solo: false, armed: false,
            notes: notes.map(([startTick, pitch, durationTicks], i): NoteSnapshot => ({
                id: String(i + 1), startTick, pitch, durationTicks, velocity: 100,
            })),
            controllerEvents: [], pitchBends: [], programChanges: [],
        }],
    };
}

const restsOf = (m: any) => m.rests.map((r: any) =>
    `${r.dotted ? 'dotted ' : ''}${r.restValue}@${r.startTick - m.startTick}`);
const notesOf = (m: any) => m.fragments.map((f: any) =>
    `${f.dotted ? 'dotted ' : ''}${f.noteValue}@${f.startTick - m.startTick}` +
    `${f.tieEnd ? '<' : ''}${f.tieStart ? '>' : ''}`);
const bars = (d: DocumentSnapshot, n = 3) => NotationService.getMeasures(d, 0, n);

// ─── Rest derivation ─────────────────────────────────────────────────────────

test('an eighth at the bar start leaves eighth + quarter + half', () => {
    // The case that used to draw one dotted-half rest and silently lose half a beat.
    const [bar] = bars(doc([[0, 67, 240]]));
    assert.deepEqual(restsOf(bar), ['eighth@240', 'quarter@480', 'half@960']);
});

test('two filled beats leave a single half rest', () => {
    const [bar] = bars(doc([[0, 67, QUARTER * 2]]));
    assert.deepEqual(restsOf(bar), ['half@960']);
});

test('a note on the last beat leaves half + quarter, not a dotted half', () => {
    // A dotted half here would hide beat 3.
    const [bar] = bars(doc([[QUARTER * 3, 67, QUARTER]]));
    assert.deepEqual(restsOf(bar), ['half@0', 'quarter@960']);
});

test('an empty bar gets one full-measure rest', () => {
    const [bar] = bars(doc([]));
    assert.equal(bar.rests.length, 1);
    assert.equal(bar.rests[0].fullMeasure, true);
    assert.equal(bar.rests[0].durationTicks, BAR_4_4);
});

test('release slack between notes does not become a rest', () => {
    // Staccato quarters: 48-tick gaps are shorter than a 32nd and are not silence.
    const [bar] = bars(doc([[0, 60, 432], [480, 62, 432], [960, 64, 432], [1440, 65, 432]]));
    assert.deepEqual(restsOf(bar), []);
});

test('compound and simple meters of the same length group differently', () => {
    const sixEight = bars(doc([[0, 67, 240]], {
        timeSignatureMap: [{ tick: 0, numerator: 6, denominator: 8 }],
    }))[0];
    const threeFour = bars(doc([[0, 67, 240]], {
        timeSignatureMap: [{ tick: 0, numerator: 3, denominator: 4 }],
    }))[0];

    assert.equal(sixEight.durationTicks, threeFour.durationTicks);
    assert.deepEqual(restsOf(sixEight),  ['eighth@240', 'eighth@480', 'dotted quarter@720']);
    assert.deepEqual(restsOf(threeFour), ['eighth@240', 'quarter@480', 'quarter@960']);
});

// ─── Note durations ──────────────────────────────────────────────────────────

test('a duration within a 32nd of a value is written as that value', () => {
    // 432 ticks is 90% of a quarter — release slack, not a dotted eighth.
    assert.equal(NotationService.snapDuration(432, PPQN), QUARTER);
    assert.equal(NotationService.snapDuration(468, PPQN), QUARTER);
    const [bar] = bars(doc([[0, 60, 432]]));
    assert.deepEqual(notesOf(bar), ['quarter@0']);
});

test('a duration further off is left alone and decomposed', () => {
    // 600 = quarter + sixteenth, too far from either to snap.
    assert.equal(NotationService.snapDuration(600, PPQN), 600);
    const [bar] = bars(doc([[0, 60, 600]]));
    assert.deepEqual(notesOf(bar), ['quarter@0>', 'sixteenth@480<']);
});

test('exact values are written as one notehead', () => {
    for (const [ticks, expected] of [
        [BAR_4_4, 'whole@0'], [QUARTER * 3, 'dotted half@0'], [QUARTER * 2, 'half@0'],
        [QUARTER, 'quarter@0'], [240, 'eighth@0'], [120, 'sixteenth@0'],
    ] as Array<[number, string]>) {
        const [bar] = bars(doc([[0, 60, ticks]]));
        assert.deepEqual(notesOf(bar), [expected], `${ticks} ticks`);
    }
});

test('a note crossing the bar line is split and tied', () => {
    const measures = bars(doc([[QUARTER * 3, 60, QUARTER * 2]]));
    assert.deepEqual(notesOf(measures[0]), ['quarter@1440>']);
    assert.deepEqual(notesOf(measures[1]), ['quarter@0<']);
});

test('notes and rests together account for the whole bar', () => {
    for (const notes of [
        [[0, 60, 240]], [[QUARTER, 60, QUARTER]], [[0, 60, 600]],
        [[0, 60, 240], [960, 64, QUARTER]], [],
    ] as Array<Array<[number, number, number]>>) {
        const [bar] = bars(doc(notes));
        const covered = bar.fragments.reduce((s, f) => s + f.durationTicks, 0) +
                        bar.rests.reduce((s, r) => s + r.durationTicks, 0);
        assert.equal(covered, BAR_4_4, `notes ${JSON.stringify(notes)}`);
    }
});

// ─── Accidentals ─────────────────────────────────────────────────────────────

test('a repeated accidental is marked once per measure', () => {
    const [bar] = bars(doc([[0, 66, 240], [240, 66, 240], [480, 66, 240], [720, 65, 240]]));
    assert.deepEqual(bar.fragments.map(f => f.accidental),
                     ['sharp', null, null, 'natural']);
});

test('the carry resets at the bar line', () => {
    const measures = bars(doc([[0, 66, 240], [BAR_4_4, 66, 240]]));
    assert.equal(measures[0].fragments[0].accidental, 'sharp');
    assert.equal(measures[1].fragments[0].accidental, 'sharp');
});

test('the key signature covers its own accidentals', () => {
    const [bar] = bars(doc([[0, 66, 240], [240, 65, 240]],
                           { keySignatureMap: [{ tick: 0, fifths: 1, minor: false }] }));
    // F# is in G major; the F natural after it needs a natural.
    assert.deepEqual(bar.fragments.map(f => f.accidental), [null, 'natural']);
});

test('a tie continuation carries no accidental', () => {
    const inC = doc([[QUARTER * 3, 66, QUARTER * 2]]);
    const measures = bars(inC);
    assert.equal(measures[0].fragments[0].accidental, 'sharp');
    assert.equal(measures[1].fragments[0].accidental, null);
    assert.equal(measures[1].fragments[0].tieEnd, true);
});

test('spelling decides the staff position, not the pitch', () => {
    const sharp = bars(doc([[0, 66, QUARTER]]))[0].fragments[0];
    const flat = bars(doc([[0, 66, QUARTER]], {
        keySignatureMap: [{ tick: 0, fifths: -6, minor: false }],
    }))[0].fragments[0];
    // Same pitch, different key: F# and Gb sit on different lines.
    assert.equal(sharp.pitch, flat.pitch);
    assert.notEqual(sharp.step, flat.step);
    assert.equal(flat.step - sharp.step, 1);
});

// ─── Measure segmentation ────────────────────────────────────────────────────

test('a mid-piece meter change re-segments the bars after it', () => {
    const measures = bars(doc([], {
        timeSignatureMap: [
            { tick: 0, numerator: 6, denominator: 8 },
            { tick: 2880, numerator: 3, denominator: 4 },
        ],
    }), 4);
    assert.deepEqual(measures.map(m => [m.startTick, m.durationTicks]),
                     [[0, 1440], [1440, 1440], [2880, 1440], [4320, 1440]]);
    assert.equal(measures[2].timeSignature.numerator, 3);
});

test('each measure resolves the key in force at its start', () => {
    const measures = bars(doc([], {
        keySignatureMap: [
            { tick: 0, fifths: -3, minor: true },
            { tick: BAR_4_4 * 2, fifths: 2, minor: false },
        ],
    }), 3);
    assert.deepEqual(measures.map(m => m.keySignature.fifths), [-3, -3, 2]);
});

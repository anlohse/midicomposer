import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DENOMINATORS, beatTicks, beatWeights, formatTimeSignature, measureAtTick,
         measureTicks, timeSignatureAt } from '../src/models/timeSignature';

const PPQN = 480;

test('only power-of-two denominators are offered', () => {
    // Standard notation admits no others, and MIDI stores log2 of the denominator.
    assert.deepEqual(DENOMINATORS, [1, 2, 4, 8, 16, 32]);
    for (const d of DENOMINATORS) {
        assert.equal(Number.isInteger(Math.log2(d)), true, `${d} must be a power of two`);
    }
});

test('measureTicks scales with the meter', () => {
    assert.equal(measureTicks({ tick: 0, numerator: 4, denominator: 4 }, PPQN), 1920);
    assert.equal(measureTicks({ tick: 0, numerator: 3, denominator: 4 }, PPQN), 1440);
    assert.equal(measureTicks({ tick: 0, numerator: 6, denominator: 8 }, PPQN), 1440);
    assert.equal(measureTicks({ tick: 0, numerator: 5, denominator: 4 }, PPQN), 2400);
    assert.equal(measureTicks({ tick: 0, numerator: 7, denominator: 8 }, PPQN), 1680);
});

test('6/8 and 3/4 are the same length but not the same meter', () => {
    const six = { tick: 0, numerator: 6, denominator: 8 };
    const three = { tick: 0, numerator: 3, denominator: 4 };
    assert.equal(measureTicks(six, PPQN), measureTicks(three, PPQN));
    assert.notEqual(six.denominator, three.denominator);
});

test('timeSignatureAt returns the meter in force and defaults to 4/4', () => {
    const map = [
        { tick: 0, numerator: 6, denominator: 8 },
        { tick: 2880, numerator: 3, denominator: 4 },
    ];
    assert.equal(timeSignatureAt(map, 0).numerator, 6);
    assert.equal(timeSignatureAt(map, 2879).numerator, 6);
    assert.equal(timeSignatureAt(map, 2880).numerator, 3);
    assert.equal(timeSignatureAt([], 0).numerator, 4);
    assert.equal(timeSignatureAt(undefined, 0).denominator, 4);
});

test('measureAtTick walks the map from the start', () => {
    const map = [{ tick: 0, numerator: 4, denominator: 4 }];
    assert.deepEqual(measureAtTick(map, PPQN, 0),
                     { index: 0, startTick: 0, numerator: 4, denominator: 4, durationTicks: 1920 });
    // Anywhere inside a bar resolves to that bar.
    assert.equal(measureAtTick(map, PPQN, 1919).index, 0);
    assert.equal(measureAtTick(map, PPQN, 1920).index, 1);
    assert.equal(measureAtTick(map, PPQN, 1920).startTick, 1920);
    assert.equal(measureAtTick(map, PPQN, 5000).index, 2);
});

test('measureAtTick follows a mid-piece meter change', () => {
    // 6/8 bars are 1440 ticks, so bar 3 starts at 2880 and is the first 3/4 bar.
    const map = [
        { tick: 0, numerator: 6, denominator: 8 },
        { tick: 2880, numerator: 3, denominator: 4 },
    ];
    assert.equal(measureAtTick(map, PPQN, 0).startTick, 0);
    assert.equal(measureAtTick(map, PPQN, 1440).index, 1);
    // The tick the core snaps a change request to: anywhere in bar 3 lands on 2880.
    for (const tick of [2880, 3000, 4319]) {
        const m = measureAtTick(map, PPQN, tick);
        assert.equal(m.startTick, 2880, `tick ${tick}`);
        assert.equal(m.index, 2);
        assert.equal(m.numerator, 3);
    }
    assert.equal(measureAtTick(map, PPQN, 4320).startTick, 4320);
});

test('formatTimeSignature reads as a meter', () => {
    assert.equal(formatTimeSignature({ numerator: 6, denominator: 8 }), '6/8');
    assert.equal(formatTimeSignature({ numerator: 4, denominator: 4 }), '4/4');
});

// ─── Bar and beat rules in the events panel ─────────────────────────────────

const FOUR_FOUR = [{ tick: 0, numerator: 4, denominator: 4 }];

test('beatTicks names the note value the denominator does', () => {
    assert.equal(beatTicks({ denominator: 4 }, PPQN), 480);
    assert.equal(beatTicks({ denominator: 8 }, PPQN), 240);
    assert.equal(beatTicks({ denominator: 2 }, PPQN), 960);
});

test('a rule opens each bar and each beat inside it', () => {
    const ticks = [0, 480, 960, 1440, 1920, 2400];
    assert.deepEqual(beatWeights(FOUR_FOUR, PPQN, ticks),
                     ['bar', 'beat', 'beat', 'beat', 'bar', 'beat']);
});

test('events between beats open nothing', () => {
    //                  downbeat  off-beat  beat 2  still beat 2
    const ticks = [0, 120, 480, 600];
    assert.deepEqual(beatWeights(FOUR_FOUR, PPQN, ticks),
                     ['bar', 'off', 'beat', 'off']);
});

test('a chord gets one rule, not one per note', () => {
    // Three notes at the same moment: the first opens the bar, the rest sit
    // under it. Drawing a line above each would be a band through the chord.
    assert.deepEqual(beatWeights(FOUR_FOUR, PPQN, [0, 0, 0, 480, 480]),
                     ['bar', 'off', 'off', 'beat', 'off']);
});

test('a bar with nothing on its downbeat still opens a bar', () => {
    // The rule marks where a bar begins in the list, so a bar whose first event
    // is late is not read as a continuation of the one before it.
    assert.deepEqual(beatWeights(FOUR_FOUR, PPQN, [0, 2100]), ['bar', 'bar']);
});

test('beats are counted from their own bar, not from tick zero', () => {
    // 7/8 bars are 1680 ticks, which is not a whole number of quarter beats —
    // counting from zero would put every later beat off the grid.
    const seven = [{ tick: 0, numerator: 7, denominator: 8 }];
    const eighth = PPQN / 2;
    assert.deepEqual(beatWeights(seven, PPQN, [0, eighth, 7 * eighth, 8 * eighth]),
                     ['bar', 'beat', 'bar', 'beat']);
});

test('a meter change starts a new bar where it lands', () => {
    const map = [{ tick: 0, numerator: 4, denominator: 4 },
                 { tick: 1920, numerator: 3, denominator: 4 }];
    // 4/4 bar, then 3/4 bars of 1440.
    assert.deepEqual(beatWeights(map, PPQN, [0, 1920, 3360, 4800]),
                     ['bar', 'bar', 'bar', 'bar']);
});

test('an empty meter map is read as four four', () => {
    assert.deepEqual(beatWeights(undefined, PPQN, [0, 480, 1920]),
                     ['bar', 'beat', 'bar']);
    assert.deepEqual(beatWeights([], PPQN, [0, 480, 1920]),
                     ['bar', 'beat', 'bar']);
});

test('a tick that goes backwards is still answered correctly', () => {
    // The walk is forward-only for speed; an unsorted list has to fall back to
    // the search rather than report nonsense.
    assert.deepEqual(beatWeights(FOUR_FOUR, PPQN, [1920, 0, 480]),
                     ['bar', 'bar', 'beat']);
});

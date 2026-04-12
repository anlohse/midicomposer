import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DENOMINATORS, formatTimeSignature, measureAtTick, measureTicks,
         timeSignatureAt } from '../src/models/timeSignature';

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

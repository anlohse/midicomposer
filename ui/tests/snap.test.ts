import { test } from 'node:test';
import assert from 'node:assert/strict';
import { SNAP_GRIDS, durationTicks, fieldCoarseStep, fieldStep, snapTicks,
         snapToGrid } from '../src/models/snap';

const PPQN = 480;

// ─── Resolutions ─────────────────────────────────────────────────────────────

test('each grid resolves to its fraction of a whole note', () => {
    const at = (grid: any) => snapTicks(true, grid, 'quarter', PPQN);
    assert.equal(at('1/4'), 480);
    assert.equal(at('1/8'), 240);
    assert.equal(at('1/16'), 120);
    assert.equal(at('1/32'), 60);
});

test('auto follows the selected note duration', () => {
    // Laying down a run of same-length notes is the common case, and it needs
    // no setup beyond picking the note value.
    assert.equal(snapTicks(true, 'auto', 'quarter', PPQN), 480);
    assert.equal(snapTicks(true, 'auto', 'eighth', PPQN), 240);
    assert.equal(snapTicks(true, 'auto', 'sixteenth', PPQN), 120);
});

test('snapping off is zero regardless of the grid', () => {
    // The grid is remembered while off, so turning snap back on returns to it.
    for (const { value } of SNAP_GRIDS) {
        assert.equal(snapTicks(false, value, 'quarter', PPQN), 0, `grid ${value}`);
    }
});

test('a resolution never collapses to zero at a small ppqn', () => {
    // Zero would read as "snapping off" everywhere downstream.
    for (const { value } of SNAP_GRIDS) {
        assert.ok(snapTicks(true, value, 'sixteenth', 4) >= 1, `grid ${value}`);
    }
});

test('durationTicks covers every offered note value', () => {
    assert.equal(durationTicks('quarter', PPQN), 480);
    assert.equal(durationTicks('eighth', PPQN), 240);
    assert.equal(durationTicks('sixteenth', PPQN), 120);
});

// ─── Rounding ────────────────────────────────────────────────────────────────

test('snapping goes to the nearest position, not the one before', () => {
    // Flooring meant a click a hair past a beat landed on the previous one,
    // which reads as notes being yanked backwards onto each other.
    assert.equal(snapToGrid(239, 480), 0);
    assert.equal(snapToGrid(241, 480), 480);
    assert.equal(snapToGrid(480, 480), 480);
});

test('with snapping off a position keeps the time it points at', () => {
    // This is what makes placement exact: only the fractional tick is dropped.
    assert.equal(snapToGrid(241, 0), 241);
    assert.equal(snapToGrid(241.4, 0), 241);
    assert.equal(snapToGrid(241.6, 0), 242);
});

test('snapping never produces a negative tick', () => {
    assert.equal(snapToGrid(-200, 480), 0);
    assert.equal(snapToGrid(-1, 0), 0);
});

// ─── Field steps ─────────────────────────────────────────────────────────────

test('a field steps by the snap resolution while snapping is on', () => {
    // The two ways of moving an event — dragging in the score and stepping in
    // the panel — have to agree, or one of them is lying about the grid.
    assert.equal(fieldStep(240), 240);
    assert.equal(fieldCoarseStep(240, PPQN), 960);
});

test('a field steps by one tick while snapping is off', () => {
    // Anything coarser would put positions out of the panel's reach that the
    // score view can place a note at.
    assert.equal(fieldStep(0), 1);
    assert.equal(fieldCoarseStep(0, PPQN), PPQN);
});

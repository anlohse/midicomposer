import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DocumentSnapshot, NoteSnapshot } from '../src/models/document';
import { MEASURE_MAX_WINDOW, MEASURE_MIN_WINDOW, MEASURE_SPARE, maxScroll,
         measureWindow, nearestStopIndex, needsReveal, scrollStops, scrollThumb,
         snapToStop, stepStop, thumbDragToScroll } from '../src/models/scoreScroll';

const PPQN = 480;
const BAR  = 4 * PPQN;   // one 4/4 measure

function note(startTick: number, durationTicks: number): NoteSnapshot {
    return { id: '1', startTick, durationTicks, pitch: 60, velocity: 100 };
}

function doc(notes: NoteSnapshot[][], meter = [{ tick: 0, numerator: 4, denominator: 4 }]):
        DocumentSnapshot {
    return {
        id: 1, title: 't', ppqn: PPQN, masterVolume: 127, revision: 1, dirty: false, filePath: '',
        canUndo: false, canRedo: false,
        tracks: notes.map((ns, i) => ({
            id: String(i + 1), name: 'T', midiChannel: 0, clef: 'treble', outputId: '',
            volume: 100, pan: 64, muted: false, solo: false, armed: false,
            notes: ns, controllerEvents: [], pitchBends: [], programChanges: [],
        })),
        tempoMap: [], timeSignatureMap: meter, keySignatureMap: [],
    };
}

// ─── measureWindow ───────────────────────────────────────────────────────────

test('an empty document still gets a usable number of measures', () => {
    assert.equal(measureWindow(doc([[]])), MEASURE_MIN_WINDOW);
    assert.equal(measureWindow(undefined), MEASURE_MIN_WINDOW);
});

test('the window covers the last note plus spare measures to write into', () => {
    // A note in bar 41 (index 40) needs 41 measures, plus the spare tail.
    const d = doc([[note(40 * BAR, PPQN)]]);
    assert.equal(measureWindow(d), 41 + MEASURE_SPARE);
});

test('a note is covered by its end, not its start', () => {
    // Starts in bar 30 and runs four bars: stopping at the start tick would
    // leave the tail of the note off the end of the score.
    const d = doc([[note(29 * BAR, 4 * BAR)]]);
    assert.equal(measureWindow(d), 33 + MEASURE_SPARE);
});

test('the window spans every track, so staves stay the same length', () => {
    const d = doc([[note(0, PPQN)], [note(60 * BAR, PPQN)]]);
    // Driven by the second track even though the first is nearly empty.
    assert.equal(measureWindow(d), 61 + MEASURE_SPARE);
});

test('the window follows the meter', () => {
    // 2/4 bars are half as long, so the same tick is twice as many measures in.
    const d = doc([[note(40 * BAR, PPQN)]],
                  [{ tick: 0, numerator: 2, denominator: 4 }]);
    assert.equal(measureWindow(d), 81 + MEASURE_SPARE);
});

test('the window is capped', () => {
    const d = doc([[note(100_000 * BAR, PPQN)]]);
    assert.equal(measureWindow(d), MEASURE_MAX_WINDOW);
});

// ─── maxScroll ───────────────────────────────────────────────────────────────

test('content that fits does not scroll', () => {
    assert.equal(maxScroll(500, 800), 0);
    assert.equal(maxScroll(800, 800), 0);
    assert.equal(maxScroll(1200, 800), 400);
});

test('a viewport of zero width never yields a negative extent', () => {
    assert.equal(maxScroll(0, 0), 0);
    assert.equal(maxScroll(300, -50), 300);
});

// ─── scrollThumb ─────────────────────────────────────────────────────────────

test('the thumb fills the track when the whole score is visible', () => {
    const { width, left } = scrollThumb(400, 300, 0, 30);
    assert.equal(width, 400);
    assert.equal(left, 0);
});

test('the thumb is proportional to the visible fraction', () => {
    // A quarter of the content on screen gives a quarter-width thumb.
    const { width } = scrollThumb(400, 1600, 0, 30);
    assert.equal(width, 100);
});

test('the thumb never shrinks below the minimum', () => {
    // True proportion here is under a pixel, which would be impossible to grab.
    const { width } = scrollThumb(400, 400_000, 0, 30);
    assert.equal(width, 30);
});

test('the thumb reaches each end of the track exactly', () => {
    const track = 400, content = 1600;
    const max = maxScroll(content, track);
    const start = scrollThumb(track, content, 0, 30);
    const end   = scrollThumb(track, content, max, 30);
    assert.equal(start.left, 0);
    // Flush with the right edge, not overhanging it.
    assert.equal(end.left + end.width, track);
});

test('the thumb position is clamped to the scrollable range', () => {
    const track = 400, content = 1600;
    const beyond = scrollThumb(track, content, 99_999, 30);
    const end    = scrollThumb(track, content, maxScroll(content, track), 30);
    assert.deepEqual(beyond, end);
    assert.equal(scrollThumb(track, content, -500, 30).left, 0);
});

test('a zero-width track produces no thumb rather than NaN', () => {
    const { width, left } = scrollThumb(0, 1600, 40, 30);
    assert.equal(width, 0);
    assert.equal(left, 0);
});

// ─── thumbDragToScroll ───────────────────────────────────────────────────────

test('dragging the thumb across the track scrolls the whole content', () => {
    const track = 400, thumb = 100, max = 1200;
    // The full travel is track - thumb; covering it must cover the full extent.
    assert.equal(thumbDragToScroll(track - thumb, track, thumb, max), max);
    assert.equal(thumbDragToScroll((track - thumb) / 2, track, thumb, max), max / 2);
    assert.equal(thumbDragToScroll(-(track - thumb), track, thumb, max), -max);
});

test('a thumb with no travel does not divide by zero', () => {
    assert.equal(thumbDragToScroll(50, 400, 400, 0), 0);
});

// ─── needsReveal ─────────────────────────────────────────────────────────────

const LABEL = 100;
const VIEW  = 900;

test('a cursor comfortably inside the note area needs no scroll', () => {
    assert.equal(needsReveal(400, LABEL, VIEW), false);
});

test('a cursor past either edge needs a scroll', () => {
    assert.equal(needsReveal(880, LABEL, VIEW), true);
    // Behind the fixed label column, so hidden even though x is positive.
    assert.equal(needsReveal(LABEL - 20, LABEL, VIEW), true);
});

test('a collapsed viewport never asks to scroll', () => {
    assert.equal(needsReveal(50, LABEL, LABEL), false);
    assert.equal(needsReveal(50, LABEL, 20), false);
});

// ─── scroll stops ────────────────────────────────────────────────────────────

// Four measure columns 400px wide, in a 500px-wide viewport.
const OFFSETS = [0, 400, 800, 1200];
const MAX = 1100;   // 1600px of content less a 500px view

test('the stops are the measure boundaries plus the end of the content', () => {
    // 1200 is past the end and is replaced by the end itself, or the last bars
    // and the right end of the scrollbar would be unreachable.
    assert.deepEqual(scrollStops(OFFSETS, MAX), [0, 400, 800, 1100]);
});

test('a score that fits has one stop', () => {
    assert.deepEqual(scrollStops(OFFSETS, 0), [0]);
});

test('the end is not duplicated when a boundary lands exactly on it', () => {
    assert.deepEqual(scrollStops([0, 400, 800], 800), [0, 400, 800]);
});

test('snapping picks the nearest stop', () => {
    const stops = scrollStops(OFFSETS, MAX);
    assert.equal(snapToStop(0, stops), 0);
    assert.equal(snapToStop(180, stops), 0);
    assert.equal(snapToStop(220, stops), 400);
    assert.equal(snapToStop(99999, stops), 1100);
    assert.equal(snapToStop(-40, stops), 0);
});

test('snapping is idempotent, so re-clamping every frame cannot drift', () => {
    const stops = scrollStops(OFFSETS, MAX);
    for (const s of stops) assert.equal(snapToStop(s, stops), s);
});

test('nearestStopIndex breaks ties towards the lower stop', () => {
    assert.equal(nearestStopIndex(200, [0, 400]), 0);
});

test('stepping moves one measure at a time', () => {
    const stops = scrollStops(OFFSETS, MAX);
    assert.equal(stepStop(0, stops, 1), 400);
    assert.equal(stepStop(400, stops, 1), 800);
    assert.equal(stepStop(800, stops, -1), 400);
    assert.equal(stepStop(0, stops, 2), 800);
});

test('stepping stops at each end instead of running off', () => {
    const stops = scrollStops(OFFSETS, MAX);
    assert.equal(stepStop(0, stops, -1), 0);
    assert.equal(stepStop(1100, stops, 1), 1100);
    assert.equal(stepStop(0, stops, 99), 1100);
});

test('stepping from between stops lands on the next one, not past it', () => {
    // Happens after a zoom, which leaves hs wherever the anchored tick put it.
    const stops = scrollStops(OFFSETS, MAX);
    assert.equal(stepStop(250, stops, 1), 400);
    assert.equal(stepStop(250, stops, -1), 0);
    assert.equal(stepStop(420, stops, 1), 800);
    assert.equal(stepStop(420, stops, -1), 400);
});

test('a single stop is a no-op to step', () => {
    assert.equal(stepStop(0, [0], 1), 0);
    assert.equal(stepStop(0, [0], -1), 0);
});

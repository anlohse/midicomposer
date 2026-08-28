// Snap resolution, shared by the score view and the events panel.
//
// Both need the same answer to "how far is one step", and they used to derive it
// separately — the score view from its grid property, the panel from a hardcoded
// sixteenth. Keeping the table here is what makes turning snap off mean the same
// thing in both places.

import type { NoteDuration } from '../components/score/score-toolbar';

/** Snap resolutions offered, other than `auto`. */
export type SnapGrid = 'auto' | '1/2' | '1/4' | '1/8' | '1/16' | '1/32';

export const SNAP_GRIDS: Array<{ value: SnapGrid; label: string }> = [
    { value: 'auto', label: 'Auto (note value)' },
    { value: '1/2',  label: '1/2' },
    { value: '1/4',  label: '1/4' },
    { value: '1/8',  label: '1/8' },
    { value: '1/16', label: '1/16' },
    { value: '1/32', label: '1/32' },
];

/**
 * A triplet is three notes in the time of two, so each one is two thirds of its
 * plain value and a group fills the next value up: three triplet quarters fill a
 * half note. Triplet mode is a modifier on the note value and the grid rather
 * than a set of extra entries in either, so the two cannot be set to disagree —
 * an eighth grid with triplets on snaps to eighth triplets, and inserting an
 * eighth inserts a triplet eighth.
 */
export const TRIPLET_RATIO = 2 / 3;

/** Plain (non-triplet) ticks for a note value. */
function plainDurationTicks(duration: NoteDuration, ppqn: number): number {
    switch (duration) {
        case 'half':      return ppqn * 2;
        case 'quarter':   return ppqn;
        case 'eighth':    return ppqn / 2;
        case 'sixteenth': return ppqn / 4;
    }
}

/**
 * Ticks in one note of the given value.
 *
 * A duration is a stored quantity, so it is rounded to whole ticks. The grid
 * below deliberately is not.
 */
export function durationTicks(duration: NoteDuration, ppqn: number,
                              triplet = false): number {
    const plain = plainDurationTicks(duration, ppqn);
    return Math.max(1, Math.round(triplet ? plain * TRIPLET_RATIO : plain));
}

/** Plain ticks per grid step, before the triplet modifier. */
function plainGridTicks(grid: SnapGrid, duration: NoteDuration, ppqn: number): number {
    switch (grid) {
        case '1/2':  return ppqn * 2;
        case '1/4':  return ppqn;
        case '1/8':  return ppqn / 2;
        case '1/16': return ppqn / 4;
        case '1/32': return ppqn / 8;
        default:     return plainDurationTicks(duration, ppqn);
    }
}

/**
 * The active snap step in ticks, or 0 when snapping is off.
 *
 * `auto` follows the selected note value, which keeps the common case — lay down
 * a run of same-length notes — aligned with no extra setup.
 *
 * The result is deliberately allowed to be fractional. A triplet step rarely
 * divides the ppqn evenly, and rounding the step would make each position drift
 * a little further than the last: at ppqn 100 a rounded triplet eighth is 33
 * ticks, so the third beat lands 4 ticks early and the tenth lands 13 early.
 * Rounding the *position* instead (see snapToGrid) keeps every one of them
 * within half a tick of where it belongs, however far into the piece it is.
 */
export function snapTicks(enabled: boolean, grid: SnapGrid, duration: NoteDuration,
                          ppqn: number, triplet = false): number {
    if (!enabled) return 0;
    const plain = plainGridTicks(grid, duration, ppqn);
    // A sub-tick step is not a grid any more, so it is floored rather than left
    // to round every position onto its neighbour.
    return Math.max(1, triplet ? plain * TRIPLET_RATIO : plain);
}

/**
 * Rounds a tick to the nearest snap position.
 *
 * Nearest, not the one before: flooring meant a click a hair past a beat still
 * landed on the previous one, which reads as notes being yanked backwards onto
 * each other. With snapping off the tick is only rounded to a whole number, so
 * a position maps to the time it actually points at.
 */
export function snapToGrid(tick: number, step: number): number {
    if (step <= 0) return Math.max(0, Math.round(tick));
    return Math.max(0, Math.round(Math.round(tick / step) * step));
}

/**
 * Quantises a length — a drag delta or a duration — to whole grid steps.
 *
 * Separate from snapToGrid because that one measures from tick 0 and this one is
 * relative: a note dragged by two triplet eighths moves by two triplet eighths
 * wherever it started. Both return whole ticks, so a fractional grid never
 * reaches the document.
 */
export function quantizeSpan(ticks: number, step: number): number {
    if (step <= 0) return Math.round(ticks);
    return Math.round(Math.round(ticks / step) * step);
}

/**
 * Step for a value field editing a tick or a duration: the snap resolution when
 * snapping is on, otherwise a single tick, so a field can address any position
 * the score view can now place a note at.
 *
 * Whole ticks, unlike the grid itself: the field shows the number it is editing,
 * and stepping by 160.5 would put a fraction of a tick on screen.
 */
export function fieldStep(snap: number): number {
    return snap > 0 ? Math.max(1, Math.round(snap)) : 1;
}

/** Coarse step (shift-held) for the same fields: a beat, or four snap steps. */
export function fieldCoarseStep(snap: number, ppqn: number): number {
    return snap > 0 ? fieldStep(snap) * 4 : ppqn;
}

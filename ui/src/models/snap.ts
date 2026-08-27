// Snap resolution, shared by the score view and the events panel.
//
// Both need the same answer to "how far is one step", and they used to derive it
// separately — the score view from its grid property, the panel from a hardcoded
// sixteenth. Keeping the table here is what makes turning snap off mean the same
// thing in both places.

import type { NoteDuration } from '../components/score/score-toolbar';

/** Snap resolutions offered, other than `auto`. */
export type SnapGrid = 'auto' | '1/4' | '1/8' | '1/16' | '1/32';

export const SNAP_GRIDS: Array<{ value: SnapGrid; label: string }> = [
    { value: 'auto', label: 'Auto (note value)' },
    { value: '1/4',  label: '1/4' },
    { value: '1/8',  label: '1/8' },
    { value: '1/16', label: '1/16' },
    { value: '1/32', label: '1/32' },
];

/** Ticks in one note of the given duration. */
export function durationTicks(duration: NoteDuration, ppqn: number): number {
    switch (duration) {
        case 'quarter':   return ppqn;
        case 'eighth':    return Math.max(1, Math.floor(ppqn / 2));
        case 'sixteenth': return Math.max(1, Math.floor(ppqn / 4));
    }
}

/**
 * The active snap step in ticks, or 0 when snapping is off.
 *
 * `auto` follows the selected note duration, which keeps the common case — lay
 * down a run of same-length notes — aligned with no extra setup.
 */
export function snapTicks(enabled: boolean, grid: SnapGrid,
                          duration: NoteDuration, ppqn: number): number {
    if (!enabled) return 0;
    switch (grid) {
        case '1/4':  return ppqn;
        case '1/8':  return Math.max(1, Math.floor(ppqn / 2));
        case '1/16': return Math.max(1, Math.floor(ppqn / 4));
        case '1/32': return Math.max(1, Math.floor(ppqn / 8));
        default:     return durationTicks(duration, ppqn);
    }
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
    return Math.max(0, Math.round(tick / step) * step);
}

/**
 * Step for a value field editing a tick or a duration: the snap resolution when
 * snapping is on, otherwise a single tick, so a field can address any position
 * the score view can now place a note at.
 */
export function fieldStep(snap: number): number {
    return snap > 0 ? snap : 1;
}

/** Coarse step (shift-held) for the same fields: a beat, or four snap steps. */
export function fieldCoarseStep(snap: number, ppqn: number): number {
    return snap > 0 ? snap * 4 : ppqn;
}

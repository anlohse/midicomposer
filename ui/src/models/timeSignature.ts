import { TimeSignatureSnapshot } from './document';

// Meter helpers shared by the notation service, the score view and the transport
// bar, so all three agree on how measures are segmented.

/** Denominators standard notation admits — and the only ones MIDI can encode. */
export const DENOMINATORS = [1, 2, 4, 8, 16, 32];

export const DEFAULT_TIME_SIGNATURE: TimeSignatureSnapshot = { tick: 0, numerator: 4, denominator: 4 };

/** Effective meter at a tick, from a map sorted by tick. */
export function timeSignatureAt(map: TimeSignatureSnapshot[] | undefined,
                                tick: number): TimeSignatureSnapshot {
    if (!map || map.length === 0) return DEFAULT_TIME_SIGNATURE;
    let latest = map[0].tick <= tick ? map[0] : DEFAULT_TIME_SIGNATURE;
    for (const ts of map) {
        if (ts.tick <= tick) latest = ts;
        else break;
    }
    return latest;
}

export function measureTicks(ts: TimeSignatureSnapshot, ppqn: number): number {
    return Math.max(1, Math.round(ppqn * 4 * ts.numerator / ts.denominator));
}

export interface MeasureInfo {
    /** 0-based; the UI shows index + 1. */
    index: number;
    startTick: number;
    numerator: number;
    denominator: number;
    durationTicks: number;
}

/**
 * The measure containing `tick`, found by walking the meter map from the start —
 * the same segmentation the notation service and the core's snapping use, so all
 * three land on identical boundaries.
 */
export function measureAtTick(map: TimeSignatureSnapshot[] | undefined,
                              ppqn: number, tick: number): MeasureInfo {
    let start = 0;
    for (let index = 0; ; index++) {
        const ts = timeSignatureAt(map, start);
        const durationTicks = measureTicks(ts, ppqn);
        if (start + durationTicks > tick || index > 100_000) {
            return { index, startTick: start, numerator: ts.numerator,
                     denominator: ts.denominator, durationTicks };
        }
        start += durationTicks;
    }
}

/**
 * Where a tick sits in its bar: on the barline, on a beat inside the bar, or
 * between beats.
 */
export type TickWeight = 'bar' | 'beat' | 'off';

/** Ticks in one beat — the note value the meter's denominator names. */
export function beatTicks(ts: { denominator: number }, ppqn: number): number {
    return Math.max(1e-9, ppqn * 4 / ts.denominator);
}

/**
 * Marks, for each tick of an ascending list, whether it opens a new bar, a new
 * beat inside the same bar, or neither.
 *
 * Openings rather than exact positions, because the caller is drawing rules
 * between rows of a list. Nothing guarantees an event sits exactly on a
 * barline, and asking only about exact hits would leave a bar with no events on
 * its downbeat looking like a continuation of the one before. Ticks that repeat
 * — several events at one moment — open nothing after the first, so a chord
 * gets a line above it and not a band through it.
 *
 * The list is walked in order, which is what keeps this linear: the single-tick
 * helpers each search from bar one, and calling them per row turns a long
 * import into a quadratic render. A tick that goes backwards is still answered
 * correctly, just by paying for that search.
 *
 * Beats are counted from the bar they are in, not from tick zero, so a bar of
 * 7/8 does not push every later beat off the grid.
 */
export function beatWeights(map: TimeSignatureSnapshot[] | undefined,
                            ppqn: number, ticks: readonly number[]): TickWeight[] {
    let bar: MeasureInfo | null = null;
    let previous: { barIndex: number; beatIndex: number } | null = null;

    return ticks.map(tick => {
        if (!bar || tick < bar.startTick) {
            bar = measureAtTick(map, ppqn, tick);
        } else {
            // Forward from where the last one landed, which is usually the same
            // bar or the next one.
            let guard = 0;
            while (tick >= bar.startTick + bar.durationTicks && guard++ < 100_000) {
                const from: MeasureInfo = bar;
                const start = from.startTick + from.durationTicks;
                const ts = timeSignatureAt(map, start);
                bar = { index: from.index + 1, startTick: start, numerator: ts.numerator,
                        denominator: ts.denominator, durationTicks: measureTicks(ts, ppqn) };
            }
        }

        // A beat can begin on a fraction of a tick when the ppqn does not divide
        // by the denominator, so a small tolerance keeps a tick that is a hair
        // under the boundary from being counted into the beat before it.
        const beatIndex = Math.floor((tick - bar.startTick) / beatTicks(bar, ppqn) + 1e-6);
        const here = { barIndex: bar.index, beatIndex };

        const weight: TickWeight =
            !previous || here.barIndex !== previous.barIndex ? 'bar'
            : here.beatIndex !== previous.beatIndex ? 'beat'
            : 'off';
        previous = here;
        return weight;
    });
}

/** "6/8", for labels. */
export function formatTimeSignature(ts: { numerator: number; denominator: number }): string {
    return `${ts.numerator}/${ts.denominator}`;
}

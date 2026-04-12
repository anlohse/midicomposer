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

/** "6/8", for labels. */
export function formatTimeSignature(ts: { numerator: number; denominator: number }): string {
    return `${ts.numerator}/${ts.denominator}`;
}

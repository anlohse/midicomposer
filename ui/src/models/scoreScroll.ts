// Horizontal scroll geometry for the score view.
//
// The score is drawn on absolutely positioned canvases, so there is no laid-out
// content for a native scroller to measure and every part of scrolling has to be
// computed. That arithmetic lives here, away from the canvas and the DOM, so it
// can be reasoned about and tested on its own.

import { DocumentSnapshot } from './document';
import { measureAtTick } from './timeSignature';

/** Empty measures always kept past the last note, to write further out into. */
export const MEASURE_SPARE = 16;
export const MEASURE_MIN_WINDOW = 32;
export const MEASURE_MAX_WINDOW = 2048;

/**
 * How many measures to lay out. Follows the material rather than being fixed: a
 * fixed window either stops short of a long piece or leaves the scrollbar thumb
 * a useless sliver on a short one.
 *
 * Counted over every track, not one, so all staves come out the same length and
 * stay aligned with each other.
 */
export function measureWindow(doc: DocumentSnapshot | undefined,
                              spare: number = MEASURE_SPARE): number {
    if (!doc) return MEASURE_MIN_WINDOW;
    let lastEnd = 0;
    for (const track of doc.tracks) {
        for (const note of track.notes) {
            lastEnd = Math.max(lastEnd, note.startTick + note.durationTicks);
        }
    }
    // End ticks are exclusive, so a note finishing on a barline belongs to the
    // measure before it and must not claim the empty one after.
    const lastSounding = Math.max(0, lastEnd - 1);
    const covered = measureAtTick(doc.timeSignatureMap, doc.ppqn, lastSounding).index + 1;
    return Math.min(MEASURE_MAX_WINDOW, Math.max(MEASURE_MIN_WINDOW, covered + spare));
}

/** Scrollable distance: content that does not fit the visible area. */
export function maxScroll(contentWidth: number, visibleWidth: number): number {
    return Math.max(0, contentWidth - Math.max(0, visibleWidth));
}

export interface ThumbGeometry {
    /** Thumb width in px; equals the track width when everything fits. */
    width: number;
    /** Thumb offset from the left of the track, in px. */
    left: number;
}

/**
 * Thumb size and position. The thumb is proportional to how much of the content
 * is on screen, but never narrower than minWidth — on a long composition the
 * true proportion would be a couple of pixels and impossible to grab.
 */
export function scrollThumb(trackWidth: number, contentWidth: number,
                            hs: number, minWidth: number): ThumbGeometry {
    if (trackWidth <= 0) return { width: 0, left: 0 };
    if (contentWidth <= 0) return { width: trackWidth, left: 0 };
    const width = Math.max(Math.min(minWidth, trackWidth),
                           Math.min(trackWidth, trackWidth * (trackWidth / contentWidth)));
    const max = maxScroll(contentWidth, trackWidth);
    if (max <= 0) return { width, left: 0 };
    const travel = trackWidth - width;
    const clamped = Math.max(0, Math.min(max, hs));
    return { width, left: (clamped / max) * travel };
}

/** Scroll delta a thumb drag of dx px means, in content px. */
export function thumbDragToScroll(dx: number, trackWidth: number,
                                  thumbWidth: number, maxHs: number): number {
    const travel = trackWidth - thumbWidth;
    if (travel <= 0) return 0;
    return dx * (maxHs / travel);
}

/** Is x far enough outside the note area that the view should move to show it? */
export function needsReveal(x: number, labelWidth: number, viewportWidth: number,
                            marginLeft = 8, marginRight = 60): boolean {
    if (viewportWidth - labelWidth <= 0) return false;
    return x < labelWidth + marginLeft || x > viewportWidth - marginRight;
}

// ─── Discrete scroll stops ───────────────────────────────────────────────────
//
// Scrolling lands on measure boundaries rather than anywhere, so the leftmost
// thing on screen is always the start of a measure. Free scrolling left half a
// bar hanging off the edge and pushed noteheads under the clef gutter.

/**
 * The positions scrolling may rest at, ascending, always starting at 0.
 *
 * `measureOffsets` are the offsets that align each measure with the start of the
 * staff; the true end of the content is added as a final stop because the last
 * measure boundary can sit short of it, and without it the last bars and the
 * right end of the scrollbar would be unreachable.
 */
export function scrollStops(measureOffsets: number[], maxHs: number): number[] {
    const inRange = measureOffsets.filter(o => o > 0 && o < maxHs);
    return [0, ...inRange, ...(maxHs > 0 ? [maxHs] : [])];
}

/** Index of the stop closest to value; ties go to the lower stop. */
export function nearestStopIndex(value: number, stops: number[]): number {
    let best = 0;
    for (let i = 1; i < stops.length; i++) {
        if (Math.abs(stops[i] - value) < Math.abs(stops[best] - value)) best = i;
    }
    return best;
}

export function snapToStop(value: number, stops: number[]): number {
    if (stops.length === 0) return value;
    return stops[nearestStopIndex(value, stops)];
}

/** Move `delta` stops from wherever `current` is. */
export function stepStop(current: number, stops: number[], delta: number): number {
    if (stops.length === 0) return current;
    let i = nearestStopIndex(current, stops);
    // Starting between two stops, one step must land on the next stop in the
    // direction of travel rather than jumping over it.
    if (delta > 0 && stops[i] > current) i -= 1;
    if (delta < 0 && stops[i] < current) i += 1;
    return stops[Math.max(0, Math.min(stops.length - 1, i + delta))];
}

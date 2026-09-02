import { NoteFragment } from './notationService';

/**
 * How the notes of a chord sit around a single stem.
 *
 * Notes that begin together are one chord, and a chord is not several notes
 * that happen to share an x. It has one stem, whose direction the whole group
 * agrees on, and two notes a diatonic second apart cannot both sit on the same
 * side of that stem -- their noteheads would occupy the same space. One of them
 * is displaced across the stem, which is the shape a reader recognises as a
 * second rather than as a smudge.
 *
 * Kept apart from the canvas so it can be tested: everything here is arithmetic
 * on staff steps, and none of it needs a rendering context.
 */

export interface ChordPlacement {
    /** Diatonic step, as `NoteFragment.step`. */
    step: number;
    /** Whether this notehead crosses to the far side of the stem. */
    displaced: boolean;
}

export interface ChordLayout {
    /** One direction for the whole chord. */
    stemDown: boolean;
    /** Same order as the steps given, so a caller can zip it back to its notes. */
    placements: ChordPlacement[];
    /** The steps the stem has to span between. */
    lowestStep: number;
    highestStep: number;
}

/**
 * Which way a chord's stem points.
 *
 * The note furthest from the middle line decides, because that is the one whose
 * stem would otherwise run off the staff. A tie -- the outer notes equally far
 * on either side -- goes down, which is the usual convention and keeps the stem
 * out of the way of anything written above.
 */
export function chordStemDown(steps: number[], middleLineStep: number): boolean {
    if (steps.length === 0) return false;
    let lowest = steps[0];
    let highest = steps[0];
    for (const step of steps) {
        if (step < lowest) lowest = step;
        if (step > highest) highest = step;
    }
    const above = highest - middleLineStep;
    const below = middleLineStep - lowest;
    return above >= below;
}

/**
 * Where each notehead goes.
 *
 * Read from the end the stem is anchored at -- the bottom for an upward stem,
 * the top for a downward one -- and displace a note whenever it is a second
 * from the one before it *and* that one stayed in place. The second condition
 * is what makes a cluster alternate instead of pushing everything across: in
 * three notes a step apart, the middle one moves and the outer two do not.
 */
export function layoutChord(
    steps: number[],
    middleLineStep: number,
    /** Forced direction. A chord under a beam follows the beam's direction
        rather than its own, and the displacement has to be worked out for the
        direction actually drawn or the seconds land on the wrong side. */
    forceStemDown?: boolean,
): ChordLayout {
    const stemDown = forceStemDown ?? chordStemDown(steps, middleLineStep);

    // Indices sorted away from the stem's anchor, so the walk below always
    // starts at the note the stem is attached to.
    const order = steps.map((_, i) => i);
    order.sort((a, b) => (stemDown ? steps[b] - steps[a] : steps[a] - steps[b]));

    const placements: ChordPlacement[] = steps.map(step => ({ step, displaced: false }));
    let previousStep: number | null = null;
    let previousDisplaced = false;
    for (const i of order) {
        const step = steps[i];
        const isSecond = previousStep !== null && Math.abs(step - previousStep) === 1;
        const displaced: boolean = isSecond && !previousDisplaced;
        placements[i].displaced = displaced;
        previousStep = step;
        previousDisplaced = displaced;
    }

    let lowestStep = steps.length ? steps[0] : 0;
    let highestStep = lowestStep;
    for (const step of steps) {
        if (step < lowestStep) lowestStep = step;
        if (step > highestStep) highestStep = step;
    }
    return { stemDown, placements, lowestStep, highestStep };
}

/**
 * The fragments of a measure, gathered into chords.
 *
 * Grouped on the onset *and* the written value: notes that start together but
 * last different lengths cannot share a stem, and in real engraving they would
 * be separate voices. Drawing them as one chord would state a duration for
 * every note in it that only one of them has, so they stay apart and each keeps
 * its own stem -- which is what the renderer did for everything until now.
 *
 * Order is preserved: the first fragment of each chord appears where it did in
 * the input, so anything downstream that walks measures in time still does.
 */
export function groupChords(fragments: NoteFragment[]): NoteFragment[][] {
    const groups: NoteFragment[][] = [];
    const byKey = new Map<string, NoteFragment[]>();

    for (const fragment of fragments) {
        const key = [
            fragment.startTick,
            fragment.noteValue,
            fragment.dotted ? 'd' : '',
            fragment.triplet ? `t${fragment.triplet.startTick}` : '',
        ].join('|');
        const existing = byKey.get(key);
        if (existing) {
            existing.push(fragment);
            continue;
        }
        const group: NoteFragment[] = [fragment];
        byKey.set(key, group);
        groups.push(group);
    }
    return groups;
}

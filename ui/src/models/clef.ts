// Clef definitions and the pitch ↔ staff-position mapping.
//
// Vertical placement is diatonic: one staff step (line → adjacent space) is
// STEP_PX, half the line gap. Everything clef-dependent collapses into a single
// number — `bottomLineStep`, the diatonic step index sitting on the bottom staff
// line — because the staff geometry itself never changes.
//
// bottomLineStep is derived as
//     step(referencePitch) - 2 * (referenceLine - 1) + 7 * octaveOffset
// e.g. treble puts G4 (step 39) on line 2 → 39 - 2 = 37, which is E4.

export type ClefName = 'treble' | 'bass' | 'alto' | 'tenor' | 'treble8va' | 'bass8vb';

export const STEP_PX = 5;          // one diatonic step in pixels
export const LINE_GAP_PX = 10;     // two steps

// C C# D D# E F F# G G# A A# B → diatonic step within the octave
const STEP_OF_SEMITONE = [0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6];
const SHARP_OF_SEMITONE = [false, true, false, true, false, false, true, false, true, false, true, false];
const NATURAL_SEMITONE_OF_STEP = [0, 2, 4, 5, 7, 9, 11];

export function pitchToStep(pitch: number): number {
    return Math.floor(pitch / 12) * 7 + STEP_OF_SEMITONE[pitch % 12];
}

export function pitchIsSharp(pitch: number): boolean {
    return SHARP_OF_SEMITONE[pitch % 12];
}

export function stepToNaturalPitch(step: number): number {
    const clamped = Math.max(0, Math.min(74, step));
    const oct = Math.floor(clamped / 7);
    const pitch = oct * 12 + NATURAL_SEMITONE_OF_STEP[clamped % 7];
    return Math.max(0, Math.min(127, pitch));
}

export interface ClefDef {
    name: ClefName;
    label: string;
    /** Diatonic step index that sits on the bottom staff line. */
    bottomLineStep: number;
    /** Base glyph; the octave marker is drawn separately. */
    glyph: string;
    /** Font size for the glyph, in px at zoom-independent staff scale. */
    fontPx: number;
    /** Glyph baseline offset from the staff's top line. */
    baselineOffset: number;
    /** '8' drawn above or below the glyph for the transposing clefs. */
    octaveMarker?: 'above' | 'below';
}

// Glyphs are the plain Unicode musical clefs — the ottava-alta/bassa codepoints
// exist but render as tofu in common system fonts, so the 8 is drawn as text.
export const CLEFS: Record<ClefName, ClefDef> = {
    treble: {
        name: 'treble', label: 'Treble (G)', bottomLineStep: 37,
        glyph: '\u{1D11E}', fontPx: 52, baselineOffset: 42,
    },
    bass: {
        name: 'bass', label: 'Bass (F)', bottomLineStep: 25,
        glyph: '\u{1D122}', fontPx: 44, baselineOffset: 34,
    },
    alto: {
        name: 'alto', label: 'Alto (C)', bottomLineStep: 31,
        glyph: '\u{1D121}', fontPx: 42, baselineOffset: 34,
    },
    tenor: {
        name: 'tenor', label: 'Tenor (C)', bottomLineStep: 29,
        glyph: '\u{1D121}', fontPx: 42, baselineOffset: 24,
    },
    treble8va: {
        name: 'treble8va', label: 'Treble 8va (G, octave up)', bottomLineStep: 44,
        glyph: '\u{1D11E}', fontPx: 52, baselineOffset: 42, octaveMarker: 'above',
    },
    bass8vb: {
        name: 'bass8vb', label: 'Bass 8vb (F, octave down)', bottomLineStep: 18,
        glyph: '\u{1D122}', fontPx: 44, baselineOffset: 34, octaveMarker: 'below',
    },
};

export const CLEF_ORDER: ClefName[] = ['treble', 'bass', 'alto', 'tenor', 'treble8va', 'bass8vb'];

export function clefDef(name: string | undefined): ClefDef {
    return CLEFS[(name as ClefName)] ?? CLEFS.treble;
}

/** Diatonic step on the staff's middle line — where stem direction flips. */
export function middleLineStep(clef: ClefDef): number {
    return clef.bottomLineStep + 4;
}

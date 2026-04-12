// Key signatures and enharmonic spelling.
//
// A key is stored as its position on the circle of fifths (-7 = Cb … 0 = C …
// +7 = C#), which is all the notation needs: it gives both the accidentals to
// draw in the signature and how to spell every pitch.

export type Alteration = -2 | -1 | 0 | 1 | 2;

/** Semitone offset of each natural letter, C D E F G A B. */
const NATURAL_SEMITONE = [0, 2, 4, 5, 7, 9, 11];
export const LETTER_NAMES = ['C', 'D', 'E', 'F', 'G', 'A', 'B'];

// The order accidentals are added to a signature, as letter indices.
const SHARP_ORDER = [3, 0, 4, 1, 5, 2, 6];   // F C G D A E B
const FLAT_ORDER  = [6, 2, 5, 1, 4, 0, 3];   // B E A D G C F

export interface KeySignature {
    tick: number;
    fifths: number;
    minor: boolean;
}

/** Alteration the signature applies to each letter, indexed C..B. */
export function keyAlterations(fifths: number): Alteration[] {
    const alt: Alteration[] = [0, 0, 0, 0, 0, 0, 0];
    const n = Math.min(Math.abs(fifths), 7);
    for (let i = 0; i < n; i++) {
        if (fifths > 0) alt[SHARP_ORDER[i]] = 1;
        else            alt[FLAT_ORDER[i]] = -1;
    }
    return alt;
}

export interface Spelling {
    /** 0..6 for C..B. */
    letter: number;
    alteration: Alteration;
    /** Absolute diatonic step index, comparable with pitchToStep in clef.ts. */
    step: number;
}

/**
 * How to write a MIDI pitch in a given key.
 *
 * A pitch that belongs to the key keeps the key's own alteration. A chromatic
 * pitch prefers the smallest alteration available — so F natural in G major is
 * written F♮, never E♯ — and only then leans the way the key leans, giving C♯ in
 * C major but D♭ in F major.
 */
export function spellPitch(pitch: number, fifths: number): Spelling {
    const alt = keyAlterations(fifths);
    const pc = ((pitch % 12) + 12) % 12;

    let letter = -1;
    let alteration: Alteration = 0;

    // In the key?
    for (let L = 0; L < 7; L++) {
        if (((NATURAL_SEMITONE[L] + alt[L]) % 12 + 12) % 12 === pc) {
            letter = L; alteration = alt[L];
            break;
        }
    }

    if (letter < 0) {
        // Chromatic: rank candidates by |alteration|, then by the key's lean.
        const prefer = fifths >= 0 ? 1 : -1;
        let best: { rank: number; tie: number; L: number; a: Alteration } | null = null;
        for (let L = 0; L < 7; L++) {
            for (const a of [-2, -1, 0, 1, 2] as Alteration[]) {
                if (((NATURAL_SEMITONE[L] + a) % 12 + 12) % 12 !== pc) continue;
                const tie = (a - alt[L]) * prefer > 0 ? 0 : 1;
                const cand = { rank: Math.abs(a), tie, L, a };
                if (!best || cand.rank < best.rank || (cand.rank === best.rank && cand.tie < best.tie)) {
                    best = cand;
                }
            }
        }
        letter = best ? best.L : 0;
        alteration = best ? best.a : 0;
    }

    // The octave is derived from where the *natural* of this letter sits, so an
    // accidental that crosses an octave boundary still lands on the right line:
    // B♯ in the octave of B, C♭ in the octave of C.
    const naturalPitch = pitch - alteration;
    const octave = Math.floor(naturalPitch / 12);
    return { letter, alteration, step: octave * 7 + letter };
}

/** MIDI pitch of a staff step as the key spells it — used for note insertion. */
export function stepToKeyPitch(step: number, fifths: number): number {
    const alt = keyAlterations(fifths);
    const clamped = Math.max(0, Math.min(74, step));
    const octave = Math.floor(clamped / 7);
    const letter = clamped % 7;
    const pitch = octave * 12 + NATURAL_SEMITONE[letter] + alt[letter];
    return Math.max(0, Math.min(127, pitch));
}

export type AccidentalGlyph = 'sharp' | 'flat' | 'natural' | 'doubleSharp' | 'doubleFlat';

const GLYPH_OF_ALTERATION: Record<number, AccidentalGlyph> = {
    2: 'doubleSharp', 1: 'sharp', 0: 'natural', [-1]: 'flat', [-2]: 'doubleFlat',
};

export const ACCIDENTAL_TEXT: Record<AccidentalGlyph, string> = {
    sharp: '♯', flat: '♭', natural: '♮',
    doubleSharp: '\u{1D12A}', doubleFlat: '\u{1D12B}',
};

/**
 * Which accidental to draw for a spelling, given what the signature already
 * says and what has already appeared at this staff position in the measure.
 * `carried` is the alteration in force, or undefined if only the key applies.
 */
export function accidentalFor(sp: Spelling, fifths: number, carried?: Alteration): AccidentalGlyph | null {
    const inForce = carried !== undefined ? carried : keyAlterations(fifths)[sp.letter];
    if (sp.alteration === inForce) return null;
    return GLYPH_OF_ALTERATION[sp.alteration] ?? null;
}

// ─── Key naming and the signature glyph layout ───────────────────────────────

const MAJOR_NAMES = ['Cb', 'Gb', 'Db', 'Ab', 'Eb', 'Bb', 'F', 'C', 'G', 'D', 'A', 'E', 'B', 'F#', 'C#'];
const MINOR_NAMES = ['Ab', 'Eb', 'Bb', 'F',  'C',  'G',  'D', 'A', 'E', 'B', 'F#', 'C#', 'G#', 'D#', 'A#'];

export function keyName(fifths: number, minor: boolean): string {
    const i = Math.max(-7, Math.min(7, fifths)) + 7;
    return minor ? `${MINOR_NAMES[i]} minor` : `${MAJOR_NAMES[i]} major`;
}

export interface KeyChoice { fifths: number; minor: boolean; label: string }

export const KEY_CHOICES: KeyChoice[] = (() => {
    const out: KeyChoice[] = [];
    for (const minor of [false, true]) {
        for (let f = -7; f <= 7; f++) {
            const accidentals = f === 0 ? '' : ` (${Math.abs(f)}${f > 0 ? '♯' : '♭'})`;
            out.push({ fifths: f, minor, label: keyName(f, minor) + accidentals });
        }
    }
    return out;
})();

// Traditional staff steps of a full 7-accidental signature in treble clef.
const TREBLE_SHARP_STEPS = [45, 42, 46, 43, 40, 44, 41];  // F5 C5 G5 D5 A4 E5 B4
const TREBLE_FLAT_STEPS  = [41, 44, 40, 43, 39, 42, 38];  // B4 E5 A4 D5 G4 C5 F4

/**
 * Where each signature accidental sits, in order, for a given clef.
 *
 * Derived by shifting the traditional treble position by the clef's bottom-line
 * difference and snapping to the nearest step with the same letter. That
 * reproduces the traditional tables exactly for treble, bass and alto, gives the
 * octave clefs the same shape an octave away, and keeps tenor on the staff.
 */
export function signatureSteps(fifths: number, bottomLineStep: number):
        { step: number; glyph: AccidentalGlyph }[] {
    const n = Math.min(Math.abs(fifths), 7);
    const sharp = fifths > 0;
    const order = sharp ? SHARP_ORDER : FLAT_ORDER;
    const base  = sharp ? TREBLE_SHARP_STEPS : TREBLE_FLAT_STEPS;
    const glyph: AccidentalGlyph = sharp ? 'sharp' : 'flat';

    const out: { step: number; glyph: AccidentalGlyph }[] = [];
    for (let i = 0; i < n; i++) {
        const letter = order[i];
        const target = base[i] + (bottomLineStep - 37);
        const octave = Math.round((target - letter) / 7);
        out.push({ step: octave * 7 + letter, glyph });
    }
    return out;
}

/**
 * Naturals that cancel the outgoing signature at a key change, at the positions
 * the accidentals they cancel used to occupy.
 *
 * Convention cancels every accidental the new key drops — so going to C major
 * writes naturals for all of them and nothing else, and a sharp key changing to a
 * flat key cancels all its sharps before the flats are written. Without this a
 * change to C major would be drawn as nothing at all.
 */
export function cancellationSteps(prevFifths: number, nextFifths: number, bottomLineStep: number):
        { step: number; glyph: AccidentalGlyph }[] {
    const nextAlt = keyAlterations(nextFifths);
    return signatureSteps(prevFifths, bottomLineStep)
        // A step's letter is its index modulo the 7 diatonic degrees.
        .filter(mark => nextAlt[((mark.step % 7) + 7) % 7] === 0)
        .map(mark => ({ step: mark.step, glyph: 'natural' as AccidentalGlyph }));
}

/**
 * How many naturals the change needs. Which letters get cancelled depends only on
 * the two keys, never on the clef, so layout can reserve width without one.
 */
export function cancellationCount(prevFifths: number, nextFifths: number): number {
    const prevAlt = keyAlterations(prevFifths);
    const nextAlt = keyAlterations(nextFifths);
    return prevAlt.reduce<number>((n, alt, letter) => n + (alt !== 0 && nextAlt[letter] === 0 ? 1 : 0), 0);
}

/** Effective key at a tick, from a map sorted by tick. */
export function keyAt(map: KeySignature[] | undefined, tick: number): KeySignature {
    const fallback: KeySignature = { tick: 0, fifths: 0, minor: false };
    if (!map || map.length === 0) return fallback;
    let best = map[0].tick <= tick ? map[0] : fallback;
    for (const ev of map) {
        if (ev.tick <= tick) best = ev;
        else break;
    }
    return best;
}

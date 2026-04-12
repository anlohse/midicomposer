import { test } from 'node:test';
import assert from 'node:assert/strict';
import { accidentalFor, cancellationCount, cancellationSteps, keyAlterations, keyAt,
         keyName, signatureSteps, spellPitch, stepToKeyPitch } from '../src/models/keySignature';
import { CLEFS, pitchToStep } from '../src/models/clef';

const LETTERS = 'CDEFGAB';
/** "F#", "Bb", "C" — how a spelling reads, for legible assertions. */
function spelt(pitch: number, fifths: number): string {
    const sp = spellPitch(pitch, fifths);
    const marks: Record<number, string> = { 2: '##', 1: '#', 0: '', [-1]: 'b', [-2]: 'bb' };
    return LETTERS[sp.letter] + marks[sp.alteration];
}
function glyph(pitch: number, fifths: number): string | null {
    return accidentalFor(spellPitch(pitch, fifths), fifths);
}

test('keyAlterations follows the circle of fifths', () => {
    assert.deepEqual(keyAlterations(0), [0, 0, 0, 0, 0, 0, 0]);
    // G major sharpens F only; F major flattens B only.
    assert.deepEqual(keyAlterations(1), [0, 0, 0, 1, 0, 0, 0]);
    assert.deepEqual(keyAlterations(-1), [0, 0, 0, 0, 0, 0, -1]);
    assert.deepEqual(keyAlterations(7), [1, 1, 1, 1, 1, 1, 1]);
    assert.deepEqual(keyAlterations(-7), [-1, -1, -1, -1, -1, -1, -1]);
});

test('a pitch in the key needs no accidental of its own', () => {
    assert.equal(spelt(66, 1), 'F#');       // F# belongs to G major
    assert.equal(glyph(66, 1), null);
    assert.equal(spelt(70, -2), 'Bb');      // Bb belongs to Bb major
    assert.equal(glyph(70, -2), null);
});

test('a chromatic pitch takes the smallest alteration, not the key direction', () => {
    // The bug this guards: leaning on the key's direction alone spells F natural
    // in G major as E#, which is musically wrong.
    assert.equal(spelt(65, 1), 'F');
    assert.equal(glyph(65, 1), 'natural');

    assert.equal(spelt(60, 2), 'C');        // C natural in D major
    assert.equal(glyph(60, 2), 'natural');
    assert.equal(spelt(64, -2), 'E');       // E natural in Bb major
    assert.equal(glyph(64, -2), 'natural');
    assert.equal(spelt(71, -1), 'B');       // B natural in F major
    assert.equal(glyph(71, -1), 'natural');
});

test('an equally-altered chromatic leans the way the key leans', () => {
    assert.equal(spelt(61, 0), 'C#');       // sharp keys and C lean sharp
    assert.equal(spelt(61, -1), 'Db');      // flat keys lean flat
    assert.equal(spelt(63, 0), 'D#');
    assert.equal(spelt(63, -1), 'Eb');
});

test('extreme keys keep their own spellings', () => {
    assert.equal(spelt(60, 7), 'B#');       // C# major writes B#, not C
    assert.equal(glyph(60, 7), null);
    assert.equal(spelt(59, -7), 'Cb');      // Cb major writes Cb, not B
    assert.equal(glyph(59, -7), null);
    assert.equal(spelt(65, 7), 'E#');       // and E#, not F natural
});

test('an accidental that crosses an octave stays on its letter\'s line', () => {
    // B#3 sounds as C4 but is written in B3's position; Cb4 sounds as B3 but is
    // written in C4's position.
    assert.equal(spellPitch(60, 7).step, pitchToStep(59));
    assert.equal(spellPitch(59, -7).step, pitchToStep(60));
});

test('accidentals carry within the measure', () => {
    const fSharp = spellPitch(66, 0);
    const fNatural = spellPitch(65, 0);
    // First F# in C major is marked; a second one at the same position is not.
    assert.equal(accidentalFor(fSharp, 0), 'sharp');
    assert.equal(accidentalFor(fSharp, 0, 1), null);
    // An F natural after it still needs cancelling.
    assert.equal(accidentalFor(fNatural, 0, 1), 'natural');
    // And in G major an F# after an F natural must be re-marked.
    assert.equal(accidentalFor(spellPitch(66, 1), 1, 0), 'sharp');
});

test('stepToKeyPitch spells the clicked staff position in the key', () => {
    const fLine = pitchToStep(65);          // the F4 line
    assert.equal(stepToKeyPitch(fLine, 1), 66);   // G major → F#4
    assert.equal(stepToKeyPitch(fLine, 0), 65);   // C major → F natural
    assert.equal(stepToKeyPitch(fLine, -1), 65);  // F major does not alter F
    const bLine = pitchToStep(71);          // the B4 line
    assert.equal(stepToKeyPitch(bLine, -1), 70);  // F major → Bb4
});

test('signature glyphs land on the traditional positions', () => {
    const names = (fifths: number, bottom: number) =>
        signatureSteps(fifths, bottom).map(m => m.step);

    // Treble, bass and alto must reproduce the engraving tables exactly.
    assert.deepEqual(names(7, CLEFS.treble.bottomLineStep), [45, 42, 46, 43, 40, 44, 41]);
    assert.deepEqual(names(7, CLEFS.bass.bottomLineStep),   [31, 28, 32, 29, 26, 30, 27]);
    assert.deepEqual(names(7, CLEFS.alto.bottomLineStep),   [38, 35, 39, 36, 33, 37, 34]);
    assert.deepEqual(names(-7, CLEFS.treble.bottomLineStep), [41, 44, 40, 43, 39, 42, 38]);
    assert.deepEqual(names(-7, CLEFS.bass.bottomLineStep),   [27, 30, 26, 29, 25, 28, 24]);
});

test('the octave clefs draw the same signature shape an octave away', () => {
    const plain = signatureSteps(4, CLEFS.treble.bottomLineStep).map(m => m.step);
    const octave = signatureSteps(4, CLEFS.treble8va.bottomLineStep).map(m => m.step);
    assert.deepEqual(octave, plain.map(s => s + 7));
});

test('signature length tracks the number of accidentals', () => {
    for (let f = -7; f <= 7; f++) {
        assert.equal(signatureSteps(f, CLEFS.treble.bottomLineStep).length, Math.abs(f));
    }
});

test('a key change cancels every accidental it drops', () => {
    // To C major: all three sharps cancelled, nothing new drawn.
    assert.equal(cancellationCount(3, 0), 3);
    assert.equal(signatureSteps(0, CLEFS.treble.bottomLineStep).length, 0);
    // Sharps to flats: every sharp cancelled before the flats appear.
    assert.equal(cancellationCount(3, -2), 3);
    // Adding accidentals cancels nothing.
    assert.equal(cancellationCount(2, 5), 0);
    assert.equal(cancellationCount(0, 4), 0);
    // Reducing sharps cancels only the ones removed.
    assert.equal(cancellationCount(5, 2), 3);

    const marks = cancellationSteps(3, 0, CLEFS.treble.bottomLineStep);
    assert.equal(marks.length, 3);
    assert.ok(marks.every(m => m.glyph === 'natural'));
    // They sit where the accidentals they cancel used to be.
    assert.deepEqual(marks.map(m => m.step),
                     signatureSteps(3, CLEFS.treble.bottomLineStep).map(m => m.step));
});

test('keyAt returns the effective key and defaults to C major', () => {
    const map = [{ tick: 0, fifths: -3, minor: true }, { tick: 3840, fifths: 2, minor: false }];
    assert.equal(keyAt(map, 0).fifths, -3);
    assert.equal(keyAt(map, 3839).fifths, -3);
    assert.equal(keyAt(map, 3840).fifths, 2);
    assert.equal(keyAt(map, 99999).fifths, 2);
    assert.equal(keyAt([], 0).fifths, 0);
    assert.equal(keyAt(undefined, 0).fifths, 0);
});

test('keys are named from the circle of fifths', () => {
    assert.equal(keyName(0, false), 'C major');
    assert.equal(keyName(0, true), 'A minor');
    assert.equal(keyName(-3, true), 'C minor');
    assert.equal(keyName(7, false), 'C# major');
    assert.equal(keyName(-7, false), 'Cb major');
});

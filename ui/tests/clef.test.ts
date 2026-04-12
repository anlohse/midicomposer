import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CLEFS, CLEF_ORDER, clefDef, middleLineStep, pitchIsSharp, pitchToStep,
         stepToNaturalPitch } from '../src/models/clef';

test('pitchToStep maps MIDI pitches onto diatonic steps', () => {
    assert.equal(pitchToStep(60), 35);            // C4
    assert.equal(pitchToStep(64), 37);            // E4
    assert.equal(pitchToStep(61), 35);            // C#4 shares C4's staff position
    assert.equal(pitchToStep(43), 25);            // G2
});

test('stepToNaturalPitch inverts pitchToStep for naturals', () => {
    for (const pitch of [43, 50, 53, 60, 64, 67, 76]) {
        assert.equal(stepToNaturalPitch(pitchToStep(pitch)), pitch, `pitch ${pitch}`);
    }
});

test('pitchIsSharp identifies the black keys', () => {
    const sharps = [1, 3, 6, 8, 10];
    for (let pc = 0; pc < 12; pc++) {
        assert.equal(pitchIsSharp(60 + pc), sharps.includes(pc), `pitch class ${pc}`);
    }
});

test('each clef puts the documented pitch on the bottom staff line', () => {
    // The whole clef model reduces to this one number per clef.
    const expected: Record<string, number> = {
        treble: 64,      // E4
        bass: 43,        // G2
        alto: 53,        // F3
        tenor: 50,       // D3
        treble8va: 76,   // E5
        bass8vb: 31,     // G1
    };
    for (const name of CLEF_ORDER) {
        assert.equal(CLEFS[name].bottomLineStep, pitchToStep(expected[name]),
                     `${name} bottom line should be MIDI ${expected[name]}`);
    }
});

test('the transposing clefs sit exactly an octave from their plain form', () => {
    assert.equal(CLEFS.treble8va.bottomLineStep - CLEFS.treble.bottomLineStep, 7);
    assert.equal(CLEFS.bass8vb.bottomLineStep - CLEFS.bass.bottomLineStep, -7);
});

test('stem direction flips two lines above the bottom', () => {
    for (const name of CLEF_ORDER) {
        assert.equal(middleLineStep(CLEFS[name]), CLEFS[name].bottomLineStep + 4);
    }
});

test('an unknown clef name falls back to treble', () => {
    assert.equal(clefDef(undefined).name, 'treble');
    assert.equal(clefDef('bogus').name, 'treble');
    assert.equal(clefDef('bass').name, 'bass');
});

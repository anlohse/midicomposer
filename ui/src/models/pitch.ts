// Shared MIDI pitch naming. Kept out of the score view so the events panel and
// the score agree on how a pitch is spelled.

const PITCH_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];

// MIDI 60 → "C4", 61 → "C♯4". Octave numbering puts middle C at C4, which
// matches the score view's staff mapping (E4 = 64 on the bottom treble line).
export function pitchName(pitch: number): string {
    const semitone = ((Math.round(pitch) % 12) + 12) % 12;
    return `${PITCH_NAMES[semitone]}${Math.floor(pitch / 12) - 1}`;
}

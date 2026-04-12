// General MIDI 1 program names, in program-number order (0-127), grouped into
// the 16 standard families of 8 so the instrument picker stays navigable.

export interface GmFamily {
    name: string;
    firstProgram: number;   // programs are firstProgram … firstProgram + 7
    programs: string[];
}

export const GM_FAMILIES: GmFamily[] = [
    { name: 'Piano', firstProgram: 0, programs: [
        'Acoustic Grand Piano', 'Bright Acoustic Piano', 'Electric Grand Piano', 'Honky-tonk Piano',
        'Electric Piano 1', 'Electric Piano 2', 'Harpsichord', 'Clavinet' ] },
    { name: 'Chromatic Percussion', firstProgram: 8, programs: [
        'Celesta', 'Glockenspiel', 'Music Box', 'Vibraphone',
        'Marimba', 'Xylophone', 'Tubular Bells', 'Dulcimer' ] },
    { name: 'Organ', firstProgram: 16, programs: [
        'Drawbar Organ', 'Percussive Organ', 'Rock Organ', 'Church Organ',
        'Reed Organ', 'Accordion', 'Harmonica', 'Tango Accordion' ] },
    { name: 'Guitar', firstProgram: 24, programs: [
        'Acoustic Guitar (nylon)', 'Acoustic Guitar (steel)', 'Electric Guitar (jazz)', 'Electric Guitar (clean)',
        'Electric Guitar (muted)', 'Overdriven Guitar', 'Distortion Guitar', 'Guitar Harmonics' ] },
    { name: 'Bass', firstProgram: 32, programs: [
        'Acoustic Bass', 'Electric Bass (finger)', 'Electric Bass (pick)', 'Fretless Bass',
        'Slap Bass 1', 'Slap Bass 2', 'Synth Bass 1', 'Synth Bass 2' ] },
    { name: 'Strings', firstProgram: 40, programs: [
        'Violin', 'Viola', 'Cello', 'Contrabass',
        'Tremolo Strings', 'Pizzicato Strings', 'Orchestral Harp', 'Timpani' ] },
    { name: 'Ensemble', firstProgram: 48, programs: [
        'String Ensemble 1', 'String Ensemble 2', 'Synth Strings 1', 'Synth Strings 2',
        'Choir Aahs', 'Voice Oohs', 'Synth Choir', 'Orchestra Hit' ] },
    { name: 'Brass', firstProgram: 56, programs: [
        'Trumpet', 'Trombone', 'Tuba', 'Muted Trumpet',
        'French Horn', 'Brass Section', 'Synth Brass 1', 'Synth Brass 2' ] },
    { name: 'Reed', firstProgram: 64, programs: [
        'Soprano Sax', 'Alto Sax', 'Tenor Sax', 'Baritone Sax',
        'Oboe', 'English Horn', 'Bassoon', 'Clarinet' ] },
    { name: 'Pipe', firstProgram: 72, programs: [
        'Piccolo', 'Flute', 'Recorder', 'Pan Flute',
        'Blown Bottle', 'Shakuhachi', 'Whistle', 'Ocarina' ] },
    { name: 'Synth Lead', firstProgram: 80, programs: [
        'Lead 1 (square)', 'Lead 2 (sawtooth)', 'Lead 3 (calliope)', 'Lead 4 (chiff)',
        'Lead 5 (charang)', 'Lead 6 (voice)', 'Lead 7 (fifths)', 'Lead 8 (bass + lead)' ] },
    { name: 'Synth Pad', firstProgram: 88, programs: [
        'Pad 1 (new age)', 'Pad 2 (warm)', 'Pad 3 (polysynth)', 'Pad 4 (choir)',
        'Pad 5 (bowed)', 'Pad 6 (metallic)', 'Pad 7 (halo)', 'Pad 8 (sweep)' ] },
    { name: 'Synth Effects', firstProgram: 96, programs: [
        'FX 1 (rain)', 'FX 2 (soundtrack)', 'FX 3 (crystal)', 'FX 4 (atmosphere)',
        'FX 5 (brightness)', 'FX 6 (goblins)', 'FX 7 (echoes)', 'FX 8 (sci-fi)' ] },
    { name: 'Ethnic', firstProgram: 104, programs: [
        'Sitar', 'Banjo', 'Shamisen', 'Koto',
        'Kalimba', 'Bag pipe', 'Fiddle', 'Shanai' ] },
    { name: 'Percussive', firstProgram: 112, programs: [
        'Tinkle Bell', 'Agogo', 'Steel Drums', 'Woodblock',
        'Taiko Drum', 'Melodic Tom', 'Synth Drum', 'Reverse Cymbal' ] },
    { name: 'Sound Effects', firstProgram: 120, programs: [
        'Guitar Fret Noise', 'Breath Noise', 'Seashore', 'Bird Tweet',
        'Telephone Ring', 'Helicopter', 'Applause', 'Gunshot' ] },
];

const FLAT_NAMES: string[] = GM_FAMILIES.flatMap(f => f.programs);

export function gmProgramName(program: number): string {
    return FLAT_NAMES[program] ?? `Program ${program}`;
}

// MIDI channel 10 (0-based 9) is the percussion channel: programs select a
// drum kit there, not a melodic instrument.
export function isPercussionChannel(channel: number): boolean {
    return channel === 9;
}

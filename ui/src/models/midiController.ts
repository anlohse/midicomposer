// MIDI control change numbers, and pitch bend presentation.
//
// Only the controllers worth naming are named: a number the user recognises is
// more useful than an exhaustive table, and everything unnamed still shows its
// number.

const NAMES: Record<number, string> = {
    0: 'Bank Select',
    1: 'Modulation',
    2: 'Breath',
    4: 'Foot',
    5: 'Portamento Time',
    7: 'Volume',
    8: 'Balance',
    10: 'Pan',
    11: 'Expression',
    64: 'Sustain',
    65: 'Portamento',
    66: 'Sostenuto',
    67: 'Soft Pedal',
    68: 'Legato',
    69: 'Hold 2',
    71: 'Resonance',
    72: 'Release Time',
    73: 'Attack Time',
    74: 'Brightness',
    84: 'Portamento Control',
    91: 'Reverb',
    93: 'Chorus',
    120: 'All Sound Off',
    121: 'Reset All Controllers',
    122: 'Local Control',
    123: 'All Notes Off',
};

/** "7 · Volume", or just the number when the controller has no common name. */
export function controllerLabel(controller: number): string {
    const name = NAMES[controller];
    return name ? `${controller} · ${name}` : String(controller);
}

export function controllerName(controller: number): string | undefined {
    return NAMES[controller];
}

/** Volume: the controller most often reached for, so the default for a new one. */
export const DEFAULT_CONTROLLER = 7;
export const DEFAULT_CONTROLLER_VALUE = 100;

export const BEND_MIN = -8192;
export const BEND_MAX = 8191;

/**
 * A bend as semitones at the default ±2 semitone range, which is what the number
 * actually means to a musician. Approximate by definition: the range is a
 * synth-side setting this application does not control.
 */
export function bendLabel(value: number): string {
    const semitones = (value / 8192) * 2;
    if (value === 0) return 'centre';
    return `${semitones >= 0 ? '+' : ''}${semitones.toFixed(2)} st`;
}

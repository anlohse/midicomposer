export interface NoteSnapshot {
    id: string;
    startTick: number;
    durationTicks: number;
    pitch: number;
    velocity: number;
}

export interface ControllerEventSnapshot {
    id: string;
    tick: number;
    controller: number;
    value: number;
}

export interface PitchBendEventSnapshot {
    id: string;
    tick: number;
    value: number;
}

export interface ProgramChangeEventSnapshot {
    id: string;
    tick: number;
    program: number;
}

export interface TrackSnapshot {
    id: string;
    name: string;
    // 0-based MIDI wire channel; the UI shows it 1-based.
    midiChannel: number;
    // Notation clef for this track's staff; see models/clef.ts.
    clef: string;
    /** Which output plays this track; empty follows the project's. */
    outputId: string;
    volume: number;
    pan: number;
    muted: boolean;
    solo: boolean;
    armed: boolean;
    notes: NoteSnapshot[];
    controllerEvents: ControllerEventSnapshot[];
    pitchBends: PitchBendEventSnapshot[];
    programChanges: ProgramChangeEventSnapshot[];
}

export interface TempoSnapshot {
    tick: number;
    bpm: number;
}

export interface KeySignatureSnapshot {
    tick: number;
    // Position on the circle of fifths: -7 = Cb … 0 = C … +7 = C#.
    fifths: number;
    minor: boolean;
}

export interface TimeSignatureSnapshot {
    tick: number;
    numerator: number;
    denominator: number;
}

export interface DocumentSnapshot {
    id: number;
    title: string;
    ppqn: number;
    /** Master fader, 0-127. Scales every track's volume on its way to MIDI. */
    masterVolume: number;
    revision: number;
    dirty: boolean;
    filePath: string;
    canUndo: boolean;
    canRedo: boolean;
    tracks: TrackSnapshot[];
    tempoMap: TempoSnapshot[];
    timeSignatureMap: TimeSignatureSnapshot[];
    keySignatureMap: KeySignatureSnapshot[];
}

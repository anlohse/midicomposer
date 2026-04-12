import { DocumentSnapshot, NoteSnapshot, TimeSignatureSnapshot,
         KeySignatureSnapshot } from '../models/document';
import { AccidentalGlyph, Alteration, accidentalFor, keyAt, spellPitch } from '../models/keySignature';
import { measureTicks, timeSignatureAt } from '../models/timeSignature';

export interface NoteFragment {
    noteId: string;
    pitch: number;
    startTick: number;
    durationTicks: number;
    tieStart: boolean;
    tieEnd: boolean;
    // Resolved notation duration. A stored note becomes several tied fragments
    // when its length has no single notatable value, so the renderer never has to
    // guess a glyph from a tick count.
    noteValue: NotationValue;
    dotted: boolean;
    // ── Notation spelling, resolved against the measure's key ────────────────
    // `step` is the absolute diatonic step index and is what decides the
    // notehead's staff position — F♯ and G♭ are the same pitch but different
    // lines, so the renderer must never derive the position from `pitch`.
    step: number;
    alteration: Alteration;
    // Accidental to draw, or null when the key signature (or an earlier
    // accidental in the same measure) already covers it.
    accidental: AccidentalGlyph | null;
}

export interface MeasureLayout {
    index: number;
    startTick: number;
    durationTicks: number;
    timeSignature: TimeSignatureSnapshot;
    keySignature: KeySignatureSnapshot;
    fragments: NoteFragment[];
    rests: RestFragment[];
}

export type NotationValue = 'whole' | 'half' | 'quarter' | 'eighth' | 'sixteenth';

export interface RestFragment {
    startTick: number;
    durationTicks: number;
    restValue: NotationValue;
    dotted: boolean;
    // Silence covering an entire measure is drawn as one centred whole rest
    // whatever the meter, so the renderer needs to know.
    fullMeasure: boolean;
}

// Every duration that can be drawn as a single notehead or rest, largest first,
// measured in quarter notes. A dotted entry is 1.5x its base value, so
// 'half' dotted = 3 quarters.
const NOTATION_VALUES: Array<{ quarters: number; value: NotationValue; dotted: boolean }> = [
    { quarters: 4,     value: 'whole',     dotted: false },
    { quarters: 3,     value: 'half',      dotted: true  },
    { quarters: 2,     value: 'half',      dotted: false },
    { quarters: 1.5,   value: 'quarter',   dotted: true  },
    { quarters: 1,     value: 'quarter',   dotted: false },
    { quarters: 0.75,  value: 'eighth',    dotted: true  },
    { quarters: 0.5,   value: 'eighth',    dotted: false },
    { quarters: 0.375, value: 'sixteenth', dotted: true  },
    { quarters: 0.25,  value: 'sixteenth', dotted: false },
];

export class NotationService {
    static getMeasures(doc: DocumentSnapshot, trackIndex: number, measureCount: number = 32): MeasureLayout[] {
        const track = doc.tracks[trackIndex];
        if (!track) return [];

        const measures: MeasureLayout[] = [];
        let currentTick = 0;

        for (let i = 0; i < measureCount; i++) {
            const timeSig = timeSignatureAt(doc.timeSignatureMap, currentTick);
            const measureDuration = measureTicks(timeSig, doc.ppqn);
            // Key changes are resolved per measure: a change part-way through a
            // bar is uncommon and would need a mid-staff signature to render.
            const keySig = keyAt(doc.keySignatureMap, currentTick);

            const measure: MeasureLayout = {
                index: i,
                startTick: currentTick,
                durationTicks: measureDuration,
                timeSignature: timeSig,
                keySignature: keySig,
                fragments: [],
                rests: []
            };

            // Accidentals hold for the rest of the measure at the staff position
            // they appeared on, so a repeated F♯ is marked once. Cleared per
            // measure, which is exactly the classical rule.
            const carried = new Map<number, Alteration>();

            const groupTicks = this.beatGroupTicks(measure, doc.ppqn);
            const measureEnd = currentTick + measureDuration;

            // Find notes that intersect this measure
            for (const note of track.notes) {
                // Snap once on the whole note, not per fragment: the amount a
                // recorded note falls short of its value is a property of the note,
                // while a fragment cut by a bar line has a genuine exact length.
                const noteEnd = note.startTick + this.snapDuration(note.durationTicks, doc.ppqn);
                if (note.startTick >= measureEnd || noteEnd <= currentTick) continue;

                const fragmentStart = Math.max(note.startTick, currentTick);
                const fragmentEnd = Math.min(noteEnd, measureEnd);
                const continuesFromPrevious = note.startTick < currentTick;
                const continuesIntoNext = noteEnd > measureEnd;

                const sp = spellPitch(note.pitch, keySig.fifths);
                // A continuation of a tie carries no accidental of its own.
                const accidental = continuesFromPrevious
                    ? null
                    : accidentalFor(sp, keySig.fifths, carried.get(sp.step));
                if (!continuesFromPrevious) carried.set(sp.step, sp.alteration);

                // One stored note becomes several tied fragments when its length
                // has no single notatable value (Notation spec §11.4).
                const pieces = this.decomposeSpan(fragmentStart, fragmentEnd, currentTick,
                                                  groupTicks, doc.ppqn, 'note');
                pieces.forEach((piece, i) => {
                    measure.fragments.push({
                        noteId: note.id,
                        pitch: note.pitch,
                        startTick: piece.startTick,
                        durationTicks: piece.durationTicks,
                        // Ties join the pieces to each other and across the bar line.
                        tieStart: i < pieces.length - 1 || continuesIntoNext,
                        tieEnd: i > 0 || continuesFromPrevious,
                        noteValue: piece.value,
                        dotted: piece.dotted,
                        step: sp.step,
                        // Only the first piece is spelled; the rest are continuations.
                        alteration: sp.alteration,
                        accidental: i === 0 ? accidental : null,
                    });
                });
            }

            measure.rests = this.calculateRests(measure, doc.ppqn);
            measures.push(measure);
            currentTick += measureDuration;
        }

        return measures;
    }

    private static calculateRests(measure: MeasureLayout, ppqn: number): RestFragment[] {
        const measureEnd = measure.startTick + measure.durationTicks;
        // Fragments arrive grouped per note, so sorting is required before the
        // occupancy merge below can rely on ascending order.
        const occupied: { start: number, end: number }[] = measure.fragments
            .map(f => ({ start: f.startTick, end: f.startTick + f.durationTicks }))
            .sort((a, b) => a.start - b.start);

        // Merge overlapping occupied ranges (simplified for MVP as we usually render them stacked)
        const merged: { start: number, end: number }[] = [];
        for (const range of occupied) {
            if (merged.length === 0 || range.start > merged[merged.length - 1].end) {
                merged.push({ ...range });
            } else {
                merged[merged.length - 1].end = Math.max(merged[merged.length - 1].end, range.end);
            }
        }

        // Gaps shorter than a 32nd are note-release slack, not musical silence —
        // imported MIDI is full of them and each one used to become a bogus
        // sixteenth rest between consecutive notes.
        const minRest = Math.max(1, Math.floor(ppqn / 8));
        const groupTicks = this.beatGroupTicks(measure, ppqn);

        const silences: Array<{ start: number, end: number }> = [];
        let current = measure.startTick;
        for (const range of merged) {
            if (range.start - current >= minRest) silences.push({ start: current, end: range.start });
            current = Math.max(current, range.end);
        }
        if (measureEnd - current >= minRest) silences.push({ start: current, end: measureEnd });

        const rests: RestFragment[] = [];
        for (const s of silences) {
            // A bar with nothing in it gets one rest, not a decomposition.
            if (s.start <= measure.startTick && s.end >= measureEnd) {
                rests.push({
                    startTick: measure.startTick, durationTicks: measure.durationTicks,
                    restValue: 'whole', dotted: false, fullMeasure: true,
                });
                continue;
            }
            rests.push(...this.decomposeSilence(s.start, s.end, measure.startTick, groupTicks, ppqn));
        }
        return rests;
    }

    // Ticks per rhythmic group.  Compound meters (6/8, 9/8, 12/8) group in
    // threes, so their beat is a dotted version of the denominator unit.
    private static beatGroupTicks(measure: MeasureLayout, ppqn: number): number {
        const { numerator, denominator } = measure.timeSignature;
        const unit = Math.max(1, Math.round(ppqn * 4 / denominator));
        const compound = numerator % 3 === 0 && numerator > 3;
        return compound ? unit * 3 : unit;
    }

    // Greedy largest-first decomposition of one silent span, per Notation
    // Rendering Rules §18.3.  A rest is only allowed at a position where it
    // reads correctly:
    //
    //   * it must start on a multiple of its own length (measure-relative), so
    //     a half rest never begins on beat 2 of 4/4
    //   * it must stay inside its beat group, unless it starts on a group
    //     boundary and spans whole groups
    //   * dotted values may never span more than one group — that is what turns
    //     three empty beats of 4/4 into half + quarter instead of one dotted
    //     half, which would hide beat 3
    private static decomposeSilence(
        spanStart: number,
        spanEnd: number,
        measureStart: number,
        groupTicks: number,
        ppqn: number,
    ): RestFragment[] {
        return this.decomposeSpan(spanStart, spanEnd, measureStart, groupTicks, ppqn, 'rest')
            .map(piece => ({
                startTick: piece.startTick, durationTicks: piece.durationTicks,
                restValue: piece.value, dotted: piece.dotted, fullMeasure: false,
            }));
    }

    /**
     * Splits a span into pieces that each map onto exactly one notatable value.
     *
     * Notes and rests follow genuinely different conventions, so the mode matters:
     *
     * - `'note'` is the greedy largest-value decomposition of Notation spec §14.2.
     *   Three beats of 4/4 are a dotted half note, and a beat and a half is a
     *   dotted quarter — writing those as tied pairs would be wrong.
     * - `'rest'` additionally requires each rest to start on a multiple of its own
     *   length and to stay inside its beat group unless it spans whole groups, and
     *   forbids dotted rests longer than a group. Those rules stop a big rest
     *   symbol from hiding where the beats fall — three empty beats of 4/4 become
     *   half + quarter, not one dotted half.
     */
    private static decomposeSpan(
        spanStart: number,
        spanEnd: number,
        measureStart: number,
        groupTicks: number,
        ppqn: number,
        mode: 'note' | 'rest',
    ): SpanPiece[] {
        const values = NOTATION_VALUES
            .map(v => ({ ...v, ticks: Math.max(1, Math.round(v.quarters * ppqn)) }))
            .filter(v => mode === 'note' || !v.dotted || v.ticks <= groupTicks);

        const out: SpanPiece[] = [];
        let t = spanStart;

        while (t < spanEnd) {
            const rem      = spanEnd - t;
            const local    = t - measureStart;
            const onGroup  = local % groupTicks === 0;
            const nextGroup = measureStart + (Math.floor(local / groupTicks) + 1) * groupTicks;

            let pick = mode === 'note'
                ? values.find(v => v.ticks <= rem)
                : values.find(v =>
                    v.ticks <= rem &&
                    local % v.ticks === 0 &&
                    (t + v.ticks <= nextGroup || (onGroup && v.ticks % groupTicks === 0)));

            // Off-grid material (imported MIDI) may satisfy no alignment rule at
            // all.  Relax to plain largest-that-fits rather than emitting one
            // oversized glyph for the whole span.
            if (!pick) pick = values.find(v => v.ticks <= rem);

            if (!pick) {
                // Remainder below a sixteenth: keep it only if it is all we have,
                // so a span is never silently dropped.
                if (out.length === 0) {
                    out.push({ startTick: t, durationTicks: rem, value: 'sixteenth', dotted: false });
                }
                break;
            }

            out.push({ startTick: t, durationTicks: pick.ticks, value: pick.value, dotted: pick.dotted });
            t += pick.ticks;
        }

        return out;
    }

    /**
     * Rounds a duration to the nearest notatable value when it is within a 32nd,
     * which absorbs the release slack that recording and MIDI import leave behind
     * — a note 90% of a quarter long is a quarter, not a dotted eighth. Anything
     * further off is left alone and gets decomposed into tied pieces instead.
     */
    static snapDuration(durationTicks: number, ppqn: number): number {
        const tolerance = Math.max(1, Math.floor(ppqn / 8));
        let best = durationTicks;
        let bestDiff = Infinity;
        for (const v of NOTATION_VALUES) {
            const ticks = Math.max(1, Math.round(v.quarters * ppqn));
            const diff = Math.abs(ticks - durationTicks);
            if (diff < bestDiff) { bestDiff = diff; best = ticks; }
        }
        return bestDiff > 0 && bestDiff <= tolerance ? best : durationTicks;
    }
}

interface SpanPiece {
    startTick: number;
    durationTicks: number;
    value: NotationValue;
    dotted: boolean;
}

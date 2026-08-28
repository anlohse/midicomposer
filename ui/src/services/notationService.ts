import { DocumentSnapshot, NoteSnapshot, TimeSignatureSnapshot,
         KeySignatureSnapshot } from '../models/document';
import { AccidentalGlyph, Alteration, accidentalFor, keyAt, spellPitch } from '../models/keySignature';
import { measureTicks, timeSignatureAt } from '../models/timeSignature';

/**
 * The group a triplet member belongs to: three notes in the time of two, filling
 * the next value up. Members of one group carry the identical span, which is
 * what lets the renderer bracket them together without re-deriving the grouping
 * from tick arithmetic it might round differently.
 */
export interface TripletSpan {
    startTick: number;
    /** Plain ticks the three notes fill between them. */
    durationTicks: number;
}

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
    // Set when this piece is one of a triplet. `noteValue` is then the value the
    // notehead is drawn as — a triplet quarter is drawn as a quarter — and the
    // span says which group it belongs to.
    triplet: TripletSpan | null;
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
    /** Set when the silence falls inside a triplet group, so the bracket covers
        it and the gap keeps its true length instead of being decomposed into
        binary rests that do not add up. */
    triplet: TripletSpan | null;
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

// The values a triplet can be written in, largest first, measured in quarter
// notes. `quarters` is the value one member is *drawn* as; three of them fill
// twice that, so a triplet quarter lasts 2/3 of a quarter and a group of three
// fills a half note.
const TRIPLET_VALUES: Array<{ quarters: number; value: NotationValue }> = [
    { quarters: 2,    value: 'half'      },
    { quarters: 1,    value: 'quarter'   },
    { quarters: 0.5,  value: 'eighth'    },
    { quarters: 0.25, value: 'sixteenth' },
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
                                                  groupTicks, doc.ppqn, 'note', 'any');
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
                        triplet: piece.triplet,
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

        // Groups the notes of this bar established. A rest is only written as a
        // triplet inside one of them: a bracketed 3 says the beat is divided in
        // three, and nothing but a note can say that. Left to position alone, an
        // ordinary silence that happens to be two thirds of a value long — the
        // second half of a bar after a triplet-quarter, say — would be bracketed
        // for no reason.
        const groups = this.tripletGroups(measure.fragments);

        const rests: RestFragment[] = [];
        for (const s of silences) {
            // A bar with nothing in it gets one rest, not a decomposition.
            if (s.start <= measure.startTick && s.end >= measureEnd) {
                rests.push({
                    startTick: measure.startTick, durationTicks: measure.durationTicks,
                    restValue: 'whole', dotted: false, triplet: null, fullMeasure: true,
                });
                continue;
            }
            // Cut at the group edges first. A silence running from inside a group
            // out the other side would otherwise be swallowed by one oversized
            // rest that hides where the group ends.
            for (const part of this.splitAtGroupEdges(s, groups)) {
                const scope = groups.find(g => part.start >= g.startTick &&
                                               part.end <= g.startTick + g.durationTicks) ?? null;
                rests.push(...this.decomposeSilence(part.start, part.end, measure.startTick,
                                                    groupTicks, ppqn, scope));
            }
        }
        return rests;
    }

    /** The distinct triplet groups the notes of a bar sit in. */
    private static tripletGroups(fragments: NoteFragment[]): TripletSpan[] {
        const byStart = new Map<number, TripletSpan>();
        for (const f of fragments) {
            if (f.triplet && !byStart.has(f.triplet.startTick)) byStart.set(f.triplet.startTick, f.triplet);
        }
        return [...byStart.values()].sort((a, b) => a.startTick - b.startTick);
    }

    /** Splits a silence wherever a triplet group begins or ends inside it. */
    private static splitAtGroupEdges(
        silence: { start: number, end: number },
        groups: TripletSpan[],
    ): Array<{ start: number, end: number }> {
        const cuts = new Set<number>();
        for (const g of groups) {
            for (const edge of [g.startTick, g.startTick + g.durationTicks]) {
                if (edge > silence.start && edge < silence.end) cuts.add(edge);
            }
        }
        const points = [silence.start, ...[...cuts].sort((a, b) => a - b), silence.end];
        const out: Array<{ start: number, end: number }> = [];
        for (let i = 0; i < points.length - 1; i++) {
            out.push({ start: points[i], end: points[i + 1] });
        }
        return out;
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
        tripletScope: TripletSpan | null,
    ): RestFragment[] {
        return this.decomposeSpan(spanStart, spanEnd, measureStart, groupTicks, ppqn,
                                  'rest', tripletScope)
            .map(piece => ({
                startTick: piece.startTick, durationTicks: piece.durationTicks,
                restValue: piece.value, dotted: piece.dotted, triplet: piece.triplet,
                fullMeasure: false,
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
        // Which triplet groups a piece here may belong to: any of them for a
        // note, one specific group for a rest inside it, none otherwise.
        tripletScope: 'any' | TripletSpan | null,
    ): SpanPiece[] {
        const values = NOTATION_VALUES
            .map(v => ({ ...v, ticks: Math.max(1, Math.round(v.quarters * ppqn)) }))
            .filter(v => mode === 'note' || !v.dotted || v.ticks <= groupTicks);

        const out: SpanPiece[] = [];
        let t = spanStart;

        while (t < spanEnd) {
            const rem      = spanEnd - t;

            // Tried before the binary values because those are picked
            // largest-that-*fits*, not largest that matches: two thirds of a half
            // note is 320 ticks at ppqn 480, and a plain quarter does not fit in
            // it, so the greedy pick would take an eighth and strand the rest.
            // A triplet length is never equal to a binary one - two thirds is
            // neither a power of two nor 3/2 times one - so looking here first
            // cannot steal a span that was going to notate cleanly.
            const trip = this.tripletPick(t, rem, measureStart, ppqn, tripletScope);
            if (trip) { out.push(trip); t += trip.durationTicks; continue; }

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
                    out.push({ startTick: t, durationTicks: rem, value: 'sixteenth',
                               dotted: false, triplet: null });
                }
                break;
            }

            out.push({ startTick: t, durationTicks: pick.ticks, value: pick.value,
                       dotted: pick.dotted, triplet: null });
            t += pick.ticks;
        }

        return out;
    }

    /**
     * Recognises one member of a triplet, or null when the span is not one.
     *
     * Both halves of the test matter. The length has to be two thirds of a plain
     * value, and the position has to be one of the three slots of a group that
     * could start there - measured from the bar line, so a meter change cannot
     * leave the slots half a beat out. Length alone would turn any oddly clipped
     * imported note into a stray bracketed 3; position alone would bracket
     * ordinary notes that happen to sit on a beat.
     *
     * Only an exact single member is claimed. A span covering two thirds of a
     * group is left to the binary path, which notates it as best it can: writing
     * it as two tied triplet members would need tie handling inside the bracket
     * that nothing else here has.
     */
    private static tripletPick(t: number, rem: number, measureStart: number,
                               ppqn: number,
                               scope: 'any' | TripletSpan | null): SpanPiece | null {
        if (scope === null) return null;

        // A recorded triplet lands a tick or two off, and at a ppqn not divisible
        // by three the exact length is not a whole number in the first place, so
        // the match cannot be by equality.
        const tolerance = Math.max(1, Math.round(ppqn / 64));

        for (const v of TRIPLET_VALUES) {
            const span = Math.round(v.quarters * 2 * ppqn);   // what the three fill
            const each = span / 3;
            if (Math.abs(rem - each) > tolerance) continue;

            const local     = t - measureStart;
            const spanStart = measureStart + Math.floor(local / span) * span;
            const slot      = Math.round((t - spanStart) / each);
            if (slot < 0 || slot > 2) continue;
            if (Math.abs((t - spanStart) - slot * each) > tolerance) continue;
            if (scope !== 'any' &&
                (scope.startTick !== spanStart || scope.durationTicks !== span)) continue;

            return {
                startTick: t,
                // The length it actually occupies, not the rounded ideal: which
                // glyph to draw is decided separately, the same split the rest of
                // this file uses.
                durationTicks: rem,
                value: v.value,
                dotted: false,
                triplet: { startTick: spanStart, durationTicks: span },
            };
        }
        return null;
    }

    /**
     * Rounds a duration to the nearest notatable value when it is close enough,
     * which absorbs the release slack that recording and MIDI import leave behind
     * — a note 90% of a quarter long is a quarter, not a dotted eighth. Anything
     * further off is left alone and gets decomposed into tied pieces instead.
     *
     * A triplet has to match twice as closely as a plain value. It is the rarer
     * reading and a much louder one on the page: a plain value that misses turns
     * into a tie, while a triplet that misses puts a bracketed 3 over a note that
     * was never part of one. At a 32nd of slack, 600 ticks would land on a
     * triplet half (640) and be bracketed, rather than reading as the quarter and
     * sixteenth it almost certainly is.
     */
    static snapDuration(durationTicks: number, ppqn: number): number {
        const plainTolerance   = Math.max(1, Math.floor(ppqn / 8));
        const tripletTolerance = Math.max(1, Math.floor(ppqn / 16));

        let best = durationTicks;
        let bestDiff = Infinity;
        // Strictly nearer, so a length equally close to a plain value and a
        // triplet is taken as the plain one.
        const consider = (ticks: number, tolerance: number) => {
            const diff = Math.abs(ticks - durationTicks);
            if (diff < bestDiff && diff <= tolerance) { bestDiff = diff; best = ticks; }
        };
        for (const v of NOTATION_VALUES) {
            consider(Math.max(1, Math.round(v.quarters * ppqn)), plainTolerance);
        }
        for (const v of TRIPLET_VALUES) {
            consider(Math.max(1, Math.round(v.quarters * 2 * ppqn / 3)), tripletTolerance);
        }
        return best;
    }
}

interface SpanPiece {
    startTick: number;
    durationTicks: number;
    value: NotationValue;
    dotted: boolean;
    triplet: TripletSpan | null;
}

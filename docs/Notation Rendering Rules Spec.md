# MIDI Composer — Notation Rendering Rules Spec

## 1. Purpose

This document defines how the UI converts the canonical musical data from the core into **visible score notation**.

It covers:

* staff and measure rendering rules
* clef, time signature, and tempo display
* note duration rendering
* tie rendering across measures
* rest derivation and rendering
* grouping rules for eighth and sixteenth notes
* how notation is derived from tick-based core data

This spec is focused on **display/rendering**, not on playback or low-level editing commands.

The goal is not to build a full engraving engine in MVP, but to define a notation system that is:

* visually correct enough for an MVP
* musically coherent
* deterministic
* extensible later

---

# 2. Scope

## 2.1 In scope

This spec covers:

* notehead/stem/flag rendering
* measure segmentation
* note splitting for visual notation
* ties across measures
* automatic rests in silent spans
* clef/time signature/tempo rendering at starts and changes
* duration interpretation from ticks
* eighth and sixteenth note display rules
* track and staff lane rendering assumptions

## 2.2 Out of scope for MVP

Not required yet:

* full professional engraving
* slur semantics distinct from ties
* ~~tuplets~~ — triplets were added after this list was written: they have a
  toolbar mode, a bracket, and they beam with their own group rather than with
  the plain notes sharing their beat
* grace notes
* complex beaming rules across every edge case
* full harmonic spelling engine
* multiple voices per staff with advanced collision layout
* lyrics
* articulation engraving
* advanced dynamic markings
* full key signature logic unless you choose to add it later

This is important: the MVP renderer should be **musically sensible**, not “Finale/Sibelius/MuseScore-level engraving.”

---

# 3. Canonical Rendering Principle

The **core model** remains canonical and stores:

* notes as absolute tick start + duration
* tempo map
* time signature map
* tracks

The **notation renderer** does not mutate the canonical model.

Instead, it derives a **visual notation model** from the core model.

That means:

* one stored note may become multiple rendered fragments
* a silent gap may become one or more rests
* visible notation is a layout transformation, not the source of truth

This separation is one of the most important architectural rules.

---

# 4. Rendering Pipeline

The notation system should operate in stages.

## 4.1 Stage 1 — Input collection

For a visible document/track/measure range, gather:

* tracks in view
* visible measure range
* notes intersecting that measure range
* time signature events affecting the range
* tempo events affecting the range
* clef or staff info affecting the range

## 4.2 Stage 2 — Measure segmentation

Determine:

* measure boundaries
* effective time signature for each measure
* effective clef for each track/measure
* effective tempo markings to show

## 4.3 Stage 3 — Note fragmentation

Convert each canonical note into one or more **measure-local notation fragments**.

This is required because:

* notes may cross measure boundaries
* visual durations may need splitting for correct notation
* ties may be needed

## 4.4 Stage 4 — Rest derivation

For each track and measure:

* determine silent gaps not occupied by visible note fragments
* derive rests for those gaps

## 4.5 Stage 5 — Symbol shaping

Convert fragments into renderable notation symbols:

* noteheads
* stems
* flags
* beams if supported
* rests
* ties
* accidentals
* clefs
* time signatures
* tempo markings

## 4.6 Stage 6 — Layout positioning

Assign each renderable symbol:

* horizontal position inside measure
* vertical staff position
* local spacing metadata

## 4.7 Stage 7 — SVG/DOM output

Render the prepared layout model into UI components, likely SVG.

---

# 5. Core Rendering Data Structures

The UI should define rendering-specific DTOs or view models rather than rendering directly from raw core entities.

## 5.1 Suggested top-level view model

```ts
interface ScoreRenderModel {
  documentId: string;
  measures: MeasureRenderModel[];
  tracks: TrackRenderModel[];
}
```

## 5.2 Track render model

```ts
interface TrackRenderModel {
  trackId: string;
  name: string;
  clef: ClefType;
  staffLanes: StaffMeasureRenderModel[];
}
```

## 5.3 Measure render model

```ts
interface StaffMeasureRenderModel {
  measureIndex: number;
  startTick: number;
  endTick: number;
  timeSignature: TimeSignatureRenderModel;
  tempoMarker?: TempoRenderModel;
  clefMarker?: ClefRenderModel;
  symbols: RenderSymbol[];
}
```

## 5.4 Symbol types

```ts
type RenderSymbol =
  | NoteSymbol
  | RestSymbol
  | TieSymbol
  | ClefSymbol
  | TimeSignatureSymbol
  | TempoSymbol
  | AccidentalSymbol;
```

The notation renderer should produce a model like this before the visual components render.

---

# 6. Staff Model

## 6.1 Staff assumption for MVP

For MVP, each track should render as one logical staff lane.

That means:

* one track = one staff lane
* notes are vertically placed according to pitch
* no advanced multi-voice staff engraving initially

This is a practical and reasonable starting point.

## 6.2 Clef model

Each track must have an effective clef for notation.

Minimum clef support recommended for MVP:

* treble clef
* bass clef

Later:

* alto/tenor/percussion

If the core does not explicitly store clef yet, the UI may derive a default based on track or pitch range, but a defined track clef is better.

## 6.3 Vertical pitch positioning

Pitch must map to vertical staff placement according to the effective clef.

The renderer should compute:

* line/space position
* whether ledger lines are required
* stem direction heuristics

---

# 7. Measure Rendering Rules

## 7.1 Measure boundaries

Measures are derived from:

* time signature map
* PPQN
* composition timeline

Every measure has:

* start tick
* end tick
* numerator
* denominator

## 7.2 Measure visual structure

Each measure should include:

* left boundary line
* notation symbols within available content width
* right boundary line
* optional change markers at the start

## 7.3 Start-of-measure visible markers

At the start of a measure, the renderer shall show:

* clef if it is the first visible measure for the staff or the clef changed
* time signature if it is the first visible measure or the time signature changed
* tempo marking if configured to show and a tempo change occurs there, or at first visible measure if desired

This directly matches your requirement that these should appear again when they change.

---

# 8. Clef Rendering Rules

## 8.1 Show clef at track start

At the beginning of a track’s visible notation, show the clef.

## 8.2 Show clef on change

If clef changes at a later measure, render the new clef at that point.

## 8.3 Clef persistence

Once shown, the clef remains effective until changed again.

## 8.4 Ledger lines

The renderer must support ledger lines for notes outside the normal staff range.

This is important even for MVP because MIDI notes can span wide ranges.

---

# 9. Time Signature Rendering Rules

## 9.1 Show at first measure

At the beginning of the first visible measure, show the effective time signature.

## 9.2 Show on change

If time signature changes at a later measure, show it again at the measure where the change becomes effective.

## 9.3 Time signature effect on rendering

Time signature affects:

* measure length in ticks
* grouping of notes and rests
* allowed beat-based decomposition
* beaming/grouping expectations for eighth and sixteenth notes

---

# 10. Tempo Rendering Rules

## 10.1 Show at score start

At the start of visible score, optionally show the effective tempo.

## 10.2 Show on tempo change

If tempo changes at a measure start, show a tempo marker there.

## 10.3 Tempo changes inside a measure

For MVP, if a tempo change occurs inside a measure:

* either show it at the precise visual point if feasible
* or constrain UI display to measure-level markers only

Recommended MVP:

* show tempo changes at the nearest meaningful measure position if exact intra-measure placement is not yet implemented visually
* preserve exact timing in the core regardless

---

# 11. Duration System

This is a foundational rendering rule.

## 11.1 Canonical duration source

Durations are stored in ticks in the core.

The renderer must convert tick durations into notation durations using:

* composition PPQN
* time signature context
* measure boundaries

## 11.2 Base note value mapping

Assuming PPQN defines quarter note duration:

* whole note = 4 quarter notes
* half note = 2 quarter notes
* quarter note = 1 quarter note
* eighth note = 1/2 quarter note
* sixteenth note = 1/4 quarter note

For example, at PPQN = 480:

* whole = 1920 ticks
* half = 960
* quarter = 480
* eighth = 240
* sixteenth = 120

## 11.3 Rendering-supported durations for MVP

At minimum, support:

* whole
* half
* quarter
* eighth
* sixteenth

Optional but useful:

* dotted half
* dotted quarter
* dotted eighth

## 11.4 Unsupported durations

If a note duration does not map cleanly to one supported notation value, the renderer must decompose it into tied fragments using supported values.

This is a very important rule.

Example:

* a duration of 720 ticks at PPQN 480
* render as quarter tied to eighth if dotted-quarter rendering is not supported
* or as dotted quarter if dotted values are supported

Recommended MVP:

* support dotted values at least for common cases
* fall back to ties when needed

---

# 12. Note Fragmentation Rules

A canonical note may need to be split visually.

## 12.1 Why fragmentation happens

A note is fragmented when:

* it crosses a measure boundary
* it cannot be represented as a single notation duration within the local beat grouping
* tie-based decomposition is required for correct notation

## 12.2 Fragmentation output

Each canonical note becomes one or more `NoteFragment`s.

Suggested shape:

```ts
interface NoteFragment {
  sourceNoteId: string;
  measureIndex: number;
  localStartTick: number;
  localDurationTicks: number;
  pitch: number;
  tieStart: boolean;
  tieContinue: boolean;
  tieEnd: boolean;
}
```

## 12.3 Fragmentation principle

Fragmentation must preserve:

* total duration
* correct measure alignment
* beat-aware notation grouping

The sum of fragment durations must exactly equal the canonical note duration.

---

# 13. Cross-Measure Tie Rules

You explicitly required correct ties across many measures.

## 13.1 Required behavior

If a note starts in one measure and continues beyond it:

* split the note visually at the measure boundary
* render a tie from the fragment in the first measure to the continuation in the next
* continue this process across as many measures as necessary

## 13.2 Multi-measure continuation

For notes spanning more than two measures:

* first fragment: tie start
* middle fragments: tie continuation
* last fragment: tie end

## 13.3 Important rule

This is only a rendering decomposition.
The underlying note in the core remains one note unless explicitly edited otherwise.

## 13.4 Tie rendering geometry

Ties should be drawn as curved connectors between adjacent noteheads or fragment endpoints.

For MVP:

* simple curved SVG path is sufficient
* exact engraving polish is not required

---

# 14. Beat-Aware Fragmentation Rules

To render notation sensibly, fragment boundaries should respect beat structure when possible.

## 14.1 Principle

A note should not be rendered as one visual unit across a beat boundary if that would make the notation musically unclear.

Example:

* in 4/4, a note starting halfway through beat 1 and lasting into beat 2 may need to be split/tied depending on duration and notation policy

## 14.2 MVP simplification

For MVP, apply these rules:

* always split at measure boundaries
* prefer split at beat boundaries when a duration cannot be represented cleanly
* use a greedy largest-valid-duration decomposition within each measure/beat segment

This gives sane notation without requiring a full engraving engine.

---

# 15. Eighth and Sixteenth Note Rendering Rules

You explicitly asked for eighth and sixteenth notes to be shown correctly based on time signature and duration in ticks.

## 15.1 Duration recognition

The renderer must identify when a note fragment corresponds to:

* eighth note
* sixteenth note

based on tick duration relative to PPQN.

## 15.2 Symbol rendering

For these durations, render:

* filled notehead
* stem
* flag count or beam grouping if implemented

Minimum acceptable MVP:

* render individual flagged notes correctly

Better MVP:

* render beam grouping inside a beat group where practical

## 15.3 Time-signature-aware grouping

Grouping should follow beat structure implied by time signature.

In 4/4:

* eighths are typically grouped by beat pairs or beat-local logic
* sixteenths grouped within beats

In 6/8:

* grouping should reflect compound meter, typically in dotted-quarter beat groups

## 15.4 MVP recommendation

To keep implementation manageable:

* first implement correct **duration symbol choice**
* then implement simple beaming/grouping rules per beat group
* fall back to flagged notes if beam grouping is not ready

That still satisfies correctness better than drawing the wrong duration.

---

# 16. Stem Rules

## 16.1 Stem presence

Notes shorter than whole note should render stems.

Whole notes have no stem.

## 16.2 Stem direction

For MVP, use a simple rule:

* notes below the middle staff line: stem up
* notes on or above the middle staff line: stem down

For chords or multi-voice layout later, this becomes more complex, but this is enough now.

---

# 17. Flag and Beam Rules

## 17.1 Minimum requirement

The renderer must visually distinguish:

* quarter notes
* eighth notes
* sixteenth notes

That means:

* eighths need one flag or one beam level
* sixteenths need two flags or two beam levels

## 17.2 MVP recommendation

Two-phase approach:

### Phase 1

Use individual flags only.

### Phase 2

Add beam grouping within beat groups.

If you want the MVP to move faster, Phase 1 is acceptable as long as durations are correct and recognizable.

---

# 18. Rest Derivation Rules

You explicitly required pauses to be calculated and drawn automatically if no note is playing in the measure.

## 18.1 Rest source rule

Rests are derived from silence, not necessarily stored in the core.

For each track and measure:

* determine time occupied by rendered note fragments
* compute the remaining silent spans
* fill those spans with rests

## 18.2 Rest computation algorithm

For each measure:

1. start with the full measure time range
2. subtract all note fragment occupied ranges
3. merge/normalize occupied intervals
4. obtain silent intervals
5. decompose each silent interval into renderable rest durations

## 18.3 Rest decomposition

Silent spans should be decomposed into standard rest values, using beat-aware grouping where practical.

For MVP:

* use a greedy decomposition into largest supported rest values
* split at measure boundaries always
* preferably split at beat boundaries when needed for clarity

## 18.4 Rest rendering requirement

If a measure contains no note activity at all, a rest representation for the full measure must be shown appropriately.

---

# 19. Overlap Policy

This needs a clear rule because MIDI data can overlap in awkward ways.

## 19.1 Monophonic vs polyphonic rendering

For MVP, assume a track may contain overlapping notes, but notation support is primarily optimized for simple melodic or lightly polyphonic material.

## 19.2 If notes overlap

Possible options:

* render simultaneous notes vertically aligned as a chord if they start together and share duration reasonably
* otherwise render independently with basic collision handling or stacked placement

Recommended MVP:

* support simple simultaneous starts as chord-like stacks later if needed
* otherwise prioritize legible independent note rendering and avoid overpromising advanced engraving

You can explicitly mark advanced polyphony layout as post-MVP.

---

# 20. Chord Rendering Rules

## 20.1 Status

Implemented. Notes that

* start at the same tick
* belong to the same track
* share the same written value — the same note value, dot and triplet membership

are one chord: one stem, one set of flags, one place under a beam.

The condition on the written value is the one that matters and the one this
section originally named. Two notes that start together and last different
lengths cannot share a stem without the stem stating a duration only one of them
has; real engraving separates them into voices, which is §2.2's post-MVP
territory. They stay as they were, each with its own stem.

## 20.2 Stem direction

One direction for the whole chord, decided by **the note furthest from the middle
line** — that is the one whose stem would otherwise run off the staff. Counting
heads instead would stem a chord upward because two of its three notes sit just
below the middle, and send the stem off the top of the staff for the third.

A chord balanced equally either side of the middle line stems down, which is the
usual convention and keeps the stem clear of whatever is written above.

Under a beam the chord follows the beam's direction rather than its own, and the
beam's direction is the majority over every *note* in the group rather than over
the chords — a three-note chord has more say in where the beam sits than a single
note beside it, because it does.

## 20.3 Seconds

Two notes a diatonic second apart cannot sit on the same side of the stem: the
noteheads would occupy the same space. One of them crosses to the far side,
which is the shape a reader recognises as a second rather than as a smudge.

Which one crosses depends on the direction. Read from the end the stem is
anchored at — the bottom for an upward stem, the top for a downward one — and
displace a note when it is a second from the previous one **and that one stayed
in place**. The second condition is what makes a cluster alternate instead of
pushing everything across: in three notes a step apart, the middle one moves and
the outer two do not.

## 20.4 Where the rules live

In `ui/src/services/chordLayout.ts`, deliberately apart from the canvas: it is
arithmetic on staff steps and needs no rendering context, so it is unit-tested
while the drawing is verified by looking at it. The renderer keeps only the
question of where to put the ink.

---

# 21. Accidental Rendering Rules

## 21.1 Basic accidental support

Render accidentals when note spelling requires them according to stored or derived notation data.

## 21.2 MVP practical rule

If note spelling metadata exists, use it.

If not, use a simple pitch-to-display rule defined by the current notation policy.

Because MIDI pitch alone does not uniquely determine enharmonic spelling, this area should remain modest in MVP.

## 21.3 Repeated accidental rules

Full classical accidental carry rules may be postponed.
For MVP, it is acceptable to render accidentals explicitly when needed by the stored/derived note spelling.

---

# 22. Horizontal Spacing Rules

## 22.1 Base principle

Horizontal position inside a measure should reflect **musical position**, not just note count.

That means symbols are placed according to local tick position within the measure.

## 22.2 Minimum spacing

Each symbol needs a minimum width so notation stays legible.

## 22.3 Measure spacing model

Recommended MVP:

* fixed or semi-proportional measure width
* within a measure, place note/rest anchors proportionally by local tick offset
* apply small collision padding around neighboring symbols

This is much simpler than full optical spacing and is fine for MVP.

---

# 23. Vertical Layout Rules

## 23.1 Staff baseline

Each track lane provides a fixed staff baseline.

## 23.2 Note vertical placement

Each note’s pitch maps to a staff position determined by clef.

## 23.3 Ledger lines

If pitch falls outside the main staff, render ledger lines.

## 23.4 Tie and accidental offsets

These should be placed relative to notehead/stem positions using simple, deterministic offsets.

---

# 24. Symbol Ordering Within a Measure

At the start of a measure, visible symbols should be ordered consistently.

Recommended order:

1. clef if needed
2. time signature if needed
3. tempo marking if shown there
4. accidentals associated with the first notes
5. notes/rests/ties across the measure body

This helps keep rendering predictable.

---

# 25. First-Visible-Measure Rules

When the UI is horizontally scrolled into the middle of a composition, the first visible measure needs special handling.

## 25.1 Rule

At the first visible measure in the viewport, the renderer should still show the currently effective clef and time signature, even if they were established earlier, if the UX chooses to prioritize readability.

This is common and useful.

## 25.2 Recommended MVP option

Support one of two modes:

* strict score continuity mode: only show actual change points
* viewport-friendly mode: repeat effective clef/time signature at viewport start

Recommended MVP:

* repeat effective clef/time signature at the first visible measure for usability, or make it a preference later

---

# 26. Notation Service Responsibilities

The notation rendering logic should live in a dedicated service.

## 26.1 Suggested service responsibilities

The `NotationService` should:

* compute visible measures
* fragment notes
* derive rests
* assign note/rest durations
* determine ties
* determine symbols to display at measure starts
* generate render-ready layout data

## 26.2 Suggested sub-services

You may split it into:

* `MeasureLayoutService`
* `NoteFragmentationService`
* `RestDerivationService`
* `PitchPlacementService`
* `TieLayoutService`

For MVP this can begin as one module and split later.

---

# 27. Suggested Rendering DTOs

## 27.1 Note symbol

```ts
interface NoteSymbol {
  kind: 'note';
  sourceNoteId: string;
  fragmentIndex: number;
  measureIndex: number;
  localStartTick: number;
  localDurationTicks: number;
  pitch: number;
  noteValue: 'whole' | 'half' | 'quarter' | 'eighth' | 'sixteenth';
  dotted: boolean;
  stemDirection?: 'up' | 'down';
  tieStart: boolean;
  tieEnd: boolean;
  x: number;
  y: number;
}
```

## 27.2 Rest symbol

```ts
interface RestSymbol {
  kind: 'rest';
  measureIndex: number;
  localStartTick: number;
  localDurationTicks: number;
  restValue: 'whole' | 'half' | 'quarter' | 'eighth' | 'sixteenth';
  dotted: boolean;
  x: number;
  y: number;
}
```

## 27.3 Tie symbol

```ts
interface TieSymbol {
  kind: 'tie';
  sourceNoteId: string;
  fromFragmentIndex: number;
  toFragmentIndex: number;
  startX: number;
  endX: number;
  y: number;
}
```

---

# 28. Fallback Rules for Difficult Cases

This keeps the renderer robust.

## 28.1 If exact notation decomposition is unclear

Fallback policy:

* preserve musical duration exactly
* prefer splitting into valid tied note values
* do not silently distort timing

## 28.2 If overlapping notation becomes cluttered

Fallback policy:

* render all notes
* accept limited visual collision in MVP
* do not lose note visibility

## 28.3 If advanced beaming is not ready

Fallback policy:

* render flagged eighth/sixteenth notes correctly as individual notes

That is much better than fake simplification.

---

# 29. Performance Rules

Because the score may scroll and update during playback, notation rendering should be incremental where possible.

## 29.1 Scope-limited rendering

Only compute/render:

* visible tracks
* visible measures
* a small buffer around viewport

## 29.2 Recompute policy

Recompute notation layout when:

* notes change in affected measures
* time signature changes
* tempo display markers change
* clef changes
* viewport changes enough to reveal new measures
* zoom changes

## 29.3 Cache policy

Cache derived render models per:

* track
* measure
* revision
* zoom level if needed

---

# 30. Example Rendering Rules

## 30.1 Quarter note entirely inside one measure

Input:

* starts at beat 2
* duration = quarter note

Output:

* one note symbol
* no tie
* no fragmentation

## 30.2 Half note crossing into next measure

Input:

* starts near measure end
* duration continues into next measure

Output:

* fragment 1 in measure N
* fragment 2 in measure N+1
* tie between them

## 30.3 Silence for half a measure

Input:

* no notes during beats 3 and 4

Output:

* one half-rest if decomposition allows
* otherwise two quarter rests if needed by current policy

## 30.4 Eighth-note run in 4/4

Input:

* consecutive eighth notes aligned to beat grid

Output:

* each rendered as eighth notes
* optionally beamed/grouped within beats if implemented
* otherwise flagged individually

---

# 31. Recommended MVP Rendering Policy

To keep implementation strong but realistic, I recommend locking these rules now:

## 31.1 Supported note values

* whole
* half
* quarter
* eighth
* sixteenth
* dotted values for common cases if feasible

## 31.2 Always split at

* measure boundaries

## 31.3 Prefer splitting at

* beat boundaries when needed for notational clarity

## 31.4 Ties

* required for all cross-measure continuations
* allowed for decomposition of unsupported single-fragment durations

## 31.5 Rests

* derived automatically from silent gaps
* shown per measure

## 31.6 Start-of-measure symbols

* clef shown at start and on change
* time signature shown at start and on change
* tempo shown at start and on tempo change, according to display policy

## 31.7 Horizontal spacing

* proportional to local measure tick position
* not full engraving spacing

## 31.8 Rendering tech

* SVG-based symbols and tie paths

That gives you a very workable and coherent MVP notation engine.

---

# 32. Final Architectural Rule

The most important rule in this whole spec is this:

**Canonical music data is not the same thing as visible notation.**

The renderer must derive notation from the core model through:

* fragmentation
* rest derivation
* measure-aware decomposition
* tie generation

Once you keep that boundary clean, the rest of the system becomes much easier to evolve.

---

# MIDI Composer — Score View Interaction Spec

## 1. Purpose

This document defines how the **Score View** behaves as an interactive editor.

It covers:

* user interaction modes
* mouse and keyboard behavior
* selection rules
* note creation and editing
* clipboard actions
* scrolling and navigation
* playback interaction
* how UI gestures are translated into core commands

This spec is focused on the **score view**, not the mixer or MIDI events panel.

---

# 2. Role of the Score View

The score view is the main musical editing surface of the application.

It must allow the user to:

* inspect the composition as written music
* navigate tracks and measures
* create notes
* select notes
* edit note timing and duration
* modify notation-related aspects such as accidentals
* cut, copy, paste, and delete notes
* follow playback visually
* switch between edit mode and play mode

The score view is not the source of truth. It is a UI editor over core-owned musical state.

---

# 3. Interaction Design Goals

The score view shall be:

* predictable
* fast to use
* visually clear
* safe against accidental destructive edits
* friendly to desktop input patterns
* capable of incremental growth later

For MVP, the interaction model should prioritize:

* clarity over cleverness
* precision over animation-heavy behavior
* simple tool semantics
* structured mapping from gestures to core commands

---

# 4. Main Modes

The score view has two top-level modes:

## 4.1 Edit mode

Used for active music editing.

Capabilities:

* create notes
* select notes
* move notes
* resize notes
* delete notes
* copy/cut/paste notes
* insert or adjust accidentals
* add tracks/measures through UI controls
* context-menu actions

The view should expose editing affordances clearly in this mode.

## 4.2 Play mode

Used for visual following during playback.

Capabilities:

* show playback cursor
* highlight active position
* optionally highlight sounding notes
* allow navigation without accidental editing

Editing affordances should be reduced or disabled in play mode unless explicitly allowed.

## 4.3 Mode switching

The current mode is part of UI state and must be clearly visible.

Recommended UI indicators:

* a segmented toggle in the score toolbar
* visual style change for active mode
* disabled editing handles in play mode

---

# 5. Tools

Within edit mode, the user interacts through a current tool.

## 5.1 Required initial tools

Recommended MVP tool set:

* Select
* Insert Note
* Resize Note
* Erase
* Sharp
* Flat
* Natural

A practical TypeScript model:

```ts
type ScoreTool =
  | 'select'
  | 'insert-note'
  | 'resize-note'
  | 'erase'
  | 'sharp'
  | 'flat'
  | 'natural';
```

## 5.2 Tool behavior principle

A tool defines how pointer actions are interpreted.

Examples:

* click with `insert-note` creates a note
* click with `select` selects an existing note or clears selection
* click with `erase` deletes the clicked note
* click with accidental tools modifies notation properties of the target note

## 5.3 Default tool

The default tool should be `select`.

That is the safest and most familiar desktop editing default.

---

# 6. Coordinate System and Hit Testing

The score view must convert pointer locations into musical targets.

## 6.1 Pointer resolution pipeline

A user click must be resolved into:

* document ID
* track ID
* measure index
* visual staff position
* beat/subdivision position
* tick position
* intended note pitch, if relevant
* target symbol or note if clicking existing content

## 6.2 Hit target categories

Pointer hit testing should distinguish at least:

* empty measure/staff area
* note glyph
* tie glyph
* accidental glyph
* measure header/start area
* track header area
* add-track button
* add-measure button
* resize handle if visible

## 6.3 Core rule

The UI may interpret a low-level click into a higher-level action, but it must not bypass the core’s authority.

Example:

* UI resolves click to `trackId=3`, `startTick=1920`, `pitch=64`, `duration=480`
* UI sends `CreateNoteCommand`
* core validates and commits
* UI updates from resulting event/response

---

# 7. Selection Model

Selection behavior must be explicit and consistent.

## 7.1 Selectable entities in score view

For MVP, the main selectable entities are:

* notes
* optionally note fragments representing tied visual pieces, but selection should still map back to the original note entity

Later this may expand to:

* rests
* measures
* symbols
* track lanes

## 7.2 Selection states

Selection should support:

* no selection
* single note selection
* multiple note selection

## 7.3 Selection storage

Selection is UI state, not core state.

## 7.4 Selection rules

### Single click on a note

Select that note.

### Single click on empty score area

Clear selection, unless modifier behavior says otherwise.

### Ctrl/Cmd-click on a note

Toggle that note in the current selection.

### Shift-click on a note

Extend selection in a meaningful way.
For MVP, this may simply add to selection.

### Drag in empty area

Creates a marquee selection rectangle.

---

# 8. Marquee Selection

## 8.1 Purpose

Allows selecting multiple notes spatially.

## 8.2 Behavior

When the `select` tool is active:

* pointer down on empty area starts marquee selection
* pointer move updates selection rectangle
* pointer up completes selection

Selected notes are those whose visual bounds intersect the marquee rectangle.

## 8.3 Modifier behavior

### No modifier

Replace current selection.

### Ctrl/Cmd

Add intersecting notes to current selection.

### Alt or other modifiers

Not needed for MVP unless clearly defined later.

---

# 9. Focus and Active Insertion Position

The score view should track an insertion anchor.

## 9.1 Insertion anchor

The insertion anchor is the current logical edit position, such as:

* active track
* active measure
* active tick
* active pitch row or staff position

This matters for:

* paste
* keyboard note insertion later
* tool previews

## 9.2 How it updates

The insertion anchor may update when:

* user clicks empty score area
* user selects a note
* playback position changes and “follow transport” is enabled, though this should not silently destroy user placement intent

---

# 10. Note Creation

This is a key interaction.

## 10.1 Insert-note tool behavior

With the `insert-note` tool active:

### Pointer move

Show a ghost preview note at the hovered position.

### Pointer click

Create a note at the resolved musical location.

The UI determines:

* target track
* start tick
* pitch
* default duration
* default velocity, if not otherwise configured

Then it sends a create command to the core.

## 10.2 Default duration

The inserted note duration should come from UI state:

* current note duration tool setting
* or last-used duration
* or a default such as quarter note

## 10.3 Grid snapping

For MVP, note insertion should snap to the active rhythmic grid.

Grid examples:

* whole
* half
* quarter
* eighth
* sixteenth

The snapped tick is what gets sent to core.

## 10.4 Click-to-create rule

A click in valid notation area with insert tool:

* resolves pitch from staff position
* resolves start tick from horizontal position
* snaps if enabled
* submits create command

---

# 11. Note Creation Preview

## 11.1 Purpose

Give visual confidence before committing.

## 11.2 Preview behavior

When hovering with insert tool:

* show translucent note at proposed position
* optionally show tie/fragment preview if current duration crosses measures
* show snapped insertion line or marker

## 11.3 Preview styling

Preview should be visually distinct from committed notes:

* lighter opacity
* dashed outline or slightly faded fill
* no selection styling

---

# 12. Note Selection and Activation

## 12.1 Clicking a note

With the `select` tool:

* click selects note
* double-click may open note details later, but not required for MVP

## 12.2 Tied note fragment selection

If the user clicks a visual fragment of a tied note spanning measures:

* the underlying original note should be selected
* all its visual fragments should show selected state

This is very important for clarity.

---

# 13. Note Movement

## 13.1 Movement behavior

With the `select` tool, selected notes may be moved by dragging.

For MVP, support moving:

* in time
* optionally across tracks if dropped onto another track lane

Pitch change by vertical movement depends on notation mapping.

## 13.2 Drag start rule

A drag begins when:

* pointer down occurs on a selected note
* pointer moves beyond a threshold distance

This avoids accidental drags from simple clicks.

## 13.3 Drag preview

During move drag:

* show ghost moved notes
* original selected notes remain visible or dimmed
* show target tick and track highlight if useful

## 13.4 Snap behavior

Moved notes should snap to the active time grid unless a modifier disables snapping.

## 13.5 Commit behavior

On pointer up:

* send batch move command to core
* do not mutate mirrored store optimistically unless you consciously want optimistic UI

Recommended MVP:

* show drag preview locally
* commit on release
* finalize from core response/events

---

# 14. Note Resize

## 14.1 Purpose

Allow changing note duration.

## 14.2 Resize affordance

Each selected note or hovered note may display a resize handle at the note’s end.

In notation view, this must be subtle and usable.
If standard notation makes resize handles awkward, they can appear only in edit mode and only on selected notes.

## 14.3 Resize tool or direct handle

Two acceptable MVP approaches:

### Approach A

Dedicated `resize-note` tool.

### Approach B

Use `select` tool and drag a note-end handle.

Recommended MVP:

* use selected-note resize handles under the select tool
* keep the separate resize tool optional

## 14.4 Resize drag rules

During resize:

* preview updated visual duration
* support snapping
* support ties if duration crosses measure boundaries
* prevent invalid zero or negative duration

On commit:

* send `ResizeNoteCommand`

---

# 15. Erase Tool

## 15.1 Behavior

With `erase` tool active:

* clicking a note deletes it immediately

## 15.2 Safety

For MVP, single-click erase is acceptable because the tool itself is explicit.

Still:

* deleted notes must be undoable
* cursor should clearly indicate erase behavior

---

# 16. Accidentals Interaction

## 16.1 Tools

Accidental tools:

* sharp
* flat
* natural

## 16.2 Behavior

When one of these tools is active and the user clicks a note:

* the UI applies the intended accidental logic
* sends an update command to core if accidentals are persisted there
* or updates notation-specific metadata if that is the chosen model

## 16.3 MVP note

If full notation semantics are not yet implemented deeply in core, the UI spec may define accidentals as notation metadata over note pitch representation rather than full harmonic spelling logic.

That keeps the architecture open without blocking MVP.

---

# 17. Clipboard Operations

## 17.1 Copy

When notes are selected and user triggers copy:

* selected notes are copied into a UI clipboard structure
* relative timing offsets should be preserved
* relative pitch/track relationships should be preserved when useful

## 17.2 Cut

Cut performs:

* copy selection to clipboard
* send delete command(s) to core

## 17.3 Paste

Paste requires an insertion anchor or suitable target location.

Behavior:

* pasted notes retain relative structure
* first pasted note aligns to insertion anchor or target point
* send batch create command to core

## 17.4 Duplicate

Worth adding to the interaction spec now:

* duplicate selection
* paste immediately after selected content or at next grid position

This is a common workflow and low-cost to support later.

---

# 18. Delete Key Behavior

## 18.1 Rule

When notes are selected and Delete or Backspace is pressed:

* send delete command for selected notes

## 18.2 No selection behavior

If nothing is selected:

* no action
* optionally soft feedback, but not required

---

# 19. Keyboard Interaction

## 19.1 Required shortcuts

In score view, support:

* Delete / Backspace — delete selected notes
* Ctrl/Cmd+C — copy
* Ctrl/Cmd+X — cut
* Ctrl/Cmd+V — paste
* Ctrl/Cmd+A — select all visible notes or all notes in current scope, depending on defined scope
* Ctrl/Cmd+Z — undo
* Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y — redo

## 19.2 Recommended additions

Useful desktop shortcuts:

* Esc — clear selection or cancel active drag/preview
* Space — play/stop if score has focus and no text input is active
* Arrow keys — navigate insertion anchor or selected note
* Shift + Arrow — extend selection or nudge note timing later

## 19.3 Focus rule

Keyboard shortcuts should only apply when:

* score view is focused
* or they are registered as global document commands

---

# 20. Context Menu Interaction

## 20.1 Right-click behavior

Right-click in score view should open a context menu appropriate to the target.

### On a selected note

Possible actions:

* Cut
* Copy
* Delete
* Resize later
* Accidental options
* Properties later

### On empty area

Possible actions:

* Paste
* Add measure
* Add track
* Insert note at cursor if tool state supports it

This is a good desktop usability feature even for MVP.

---

# 21. Scroll Behavior

## 21.1 Horizontal scroll

The score view scrolls horizontally through measures.

Required uses:

* navigate long compositions
* reveal later measures
* follow playback in play mode if enabled

## 21.2 Vertical scroll

The score view scrolls vertically through tracks.

Required uses:

* compositions with many tracks
* keeping score area compact but scalable

## 21.3 Pointer wheel behavior

Recommended:

* vertical wheel scrolls vertically
* Shift + wheel scrolls horizontally
* trackpad gestures should work naturally where available

## 21.4 Auto-scroll during playback

In play mode, if follow-playback is enabled:

* the view should auto-scroll horizontally to keep playback cursor visible

This should be configurable in preferences.

---

# 22. Zoom Behavior

Even if not explicitly required before, zoom is useful enough to define now.

## 22.1 Types of zoom

Recommended:

* horizontal zoom for measure density
* notation scale zoom for visual readability

## 22.2 Interaction

Possible controls:

* toolbar zoom buttons
* Ctrl/Cmd + mouse wheel
* keyboard shortcuts later

## 22.3 Constraints

Zoom must not change underlying musical timing, only rendering scale.

---

# 23. Playback Cursor Interaction

## 23.1 Display

During playback, show a moving playback cursor in the score.

## 23.2 Behavior in play mode

Play mode emphasizes:

* playback cursor
* current measure/beat awareness
* optional note highlighting

## 23.3 Behavior in edit mode during playback

The app may still allow playback while editing.
If so:

* playback cursor appears
* editing remains possible only if safe
* accidental edits should still be prevented where interaction conflicts are likely

For MVP, a simpler rule is acceptable:

* playback allowed in edit mode
* but score interactions are reduced or disabled while dragging/recording

---

# 24. Current Position and Seeking Interaction

## 24.1 Click-to-seek

In play mode, clicking a measure or location may move the playhead.

This is a very useful behavior and should be supported if feasible.

## 24.2 Recommended rule

With play mode active:

* click empty score area = seek transport to clicked position

With edit mode active:

* click empty score area = selection clear / insertion anchor change
* do not seek unless a dedicated modifier or control is used

That avoids confusion.

---

# 25. Add Track Interaction

## 25.1 Add track button

A visible `(+)` control near the track header region creates a new track.

## 25.2 Behavior

On click:

* UI sends create-track command
* new track appears in score and mixer

## 25.3 New track placement

For MVP:

* append at end of track list

Later:

* support inserting at specific position

---

# 26. Add Measure Interaction

## 26.1 Add measure button

A visible `(+)` control at the far right of the score header or measure row adds visible measure space.

## 26.2 Behavior

On click:

* UI sends an extend-composition or add-measure style command
* timeline expands accordingly

## 26.3 Important architecture note

Measures remain timeline-derived in core, but the user experience may still expose “add measure” as a practical editing action.

---

# 27. Playback vs Editing Conflict Rules

These need to be explicit.

## 27.1 While playing

Allowed:

* passive navigation
* selection
* possibly seek
* maybe some non-destructive actions

Potentially restricted:

* destructive edits
* drag-resize interactions
* batch move operations during unstable playback states

## 27.2 While recording

More restricted.
Recommended MVP:

* disable most score edits during active recording
* still show playback cursor and newly committed notes

## 27.3 Rationale

This reduces race conditions and confusing state changes.

---

# 28. Undo/Redo Interaction

## 28.1 Rule

All score editing operations must be undoable.

This includes:

* create
* move
* resize
* delete
* accidental changes if persisted
* paste
* cut

## 28.2 Gesture grouping

A drag move or drag resize should create one undoable operation, not hundreds of tiny ones.

This means:

* preview locally during drag
* commit as one command or one transaction on release

---

# 29. Visual Feedback Rules

## 29.1 Hover feedback

Hovering should provide light feedback:

* hovered note highlight
* hovered insertion target preview
* hovered track/measure cue if useful

## 29.2 Selected state

Selected notes must be clearly distinguishable from:

* normal notes
* hovered notes
* preview notes

## 29.3 Invalid action feedback

If an action is not valid:

* do not silently fail
* either ignore with clear visual indication or show a lightweight message

Examples:

* paste with no valid target
* resize below minimum duration
* edit while recording when disallowed

---

# 30. Gesture-to-Core Mapping

This is central to the architecture.

## 30.1 General rule

The UI interprets pointer/keyboard gestures into semantic commands.

Examples:

### Insert click

becomes:

* `CreateNoteCommand`

### Drag move

becomes:

* `BatchEditCommand` containing note move operations
  or
* `MoveNoteCommand` for single-note edit

### Resize drag

becomes:

* `ResizeNoteCommand`

### Delete key

becomes:

* delete command(s)

### Add track

becomes:

* `CreateTrackCommand`

### Add measure

becomes:

* composition extension command

## 30.2 UI preprocessing allowed

The UI may perform minimal preprocessing such as:

* hit testing
* snapping
* pitch resolution
* duration selection
* gesture grouping

But final validation belongs to the core.

---

# 31. Interaction State Machine

A simple internal interaction state machine will help implementation.

## 31.1 Suggested states

```ts
type ScoreInteractionState =
  | { kind: 'idle' }
  | { kind: 'hovering'; target: HoverTarget }
  | { kind: 'marquee-select'; start: Point; current: Point }
  | { kind: 'dragging-notes'; noteIds: string[]; anchor: Point; preview: DragPreview }
  | { kind: 'resizing-note'; noteId: string; anchor: Point; preview: ResizePreview }
  | { kind: 'inserting-note-preview'; target: InsertTarget }
  | { kind: 'context-menu-open'; target: MenuTarget };
```

This makes behavior easier to reason about than scattered flags.

---

# 32. Score Toolbar Interaction

The score toolbar should expose:

* current mode: edit/play
* current tool
* note duration selection
* accidental tool selection
* snap/grid setting
* zoom controls
* add track / add measure shortcuts if desired

This toolbar anchors the interaction model and reduces ambiguity.

---

# 33. Notation-Specific Interaction Rules

Because this is score view rather than piano roll, notation logic matters.

## 33.1 Cross-measure visual editing

When a user edits a note that spans multiple measures:

* the UI manipulates the original note entity
* the notation service updates fragments/ties for display

## 33.2 Rest interaction

For MVP, rests are derived visual elements, not directly editable as standalone objects.

User editing silence is done by:

* inserting/deleting/resizing notes
* rests recompute automatically

That is the cleanest rule.

## 33.3 Clef/time signature/tempo display

These are visible notation markers.
Click interaction on them may later support editing, but that can be deferred.
For MVP they are primarily informative.

---

# 34. Empty State Interaction

## 34.1 Empty document

If the document has no tracks or no visible notes:

* score view should still render usable empty structure
* clear affordances should allow adding a track and starting note insertion

## 34.2 First-run behavior

A helpful MVP default:

* create one default track in a new document
* position insertion at measure 1
* default to select tool, not insert tool

That makes the UI feel less blank.

---

# 35. Accessibility and Usability Notes

Even in a desktop music app, a few basic rules matter:

* hit targets should not be tiny
* selected state should not rely on color alone
* keyboard shortcuts should cover core editing actions
* hover-only affordances should still have selection/focus alternatives

No need to overdesign accessibility for MVP, but don’t paint yourself into a corner.

---

# 36. Recommended MVP Interaction Scope

To keep the first implementation realistic, I would define MVP score interaction as:

## 36.1 Must-have

* edit/play mode toggle
* select tool
* insert-note tool
* erase tool
* single and multi-select
* marquee select
* note create
* note move
* note resize
* delete
* copy/cut/paste
* horizontal/vertical scroll
* playback cursor
* click-to-seek in play mode
* add track
* add measure
* tied visual rendering for cross-measure notes
* automatic rest rendering
* eighth and sixteenth note visual rendering based on duration/time signature

## 36.2 Can wait slightly

* advanced accidental editing semantics
* keyboard note entry
* drag-copy with modifier
* deep context menu options
* direct editing of clef/time signature/tempo markers from score

That keeps the MVP strong without exploding scope.

---

# 37. Final Recommended Interaction Rules to Lock Now

Here’s the concise set I would lock in:

* **Default mode:** edit
* **Default tool:** select
* **Insert behavior:** click with insert-note tool creates snapped note at hovered pitch/time
* **Selection:** click, Ctrl/Cmd-click, marquee
* **Move:** drag selected notes
* **Resize:** drag selected note end handle
* **Delete:** erase tool or Delete key
* **Clipboard:** copy/cut/paste from selection
* **Play mode click:** seek transport
* **Edit mode click on empty area:** update insertion anchor / clear selection
* **Cross-measure notes:** visually split with ties, but keep one underlying note entity
* **Rests:** derived automatically, not manually stored for MVP
* **Playback updates:** cursor shown, auto-scroll optional
* **Recording:** score mostly read-only during active recording

---

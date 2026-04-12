# MIDI Composer — UI Architecture and Screen/Component Spec

## 1. Purpose

This document defines the architecture and behavior of the **UI layer** for the MIDI Composer desktop application.

The UI is responsible for:

* rendering the composition visually
* allowing the user to edit notes, tracks, measures, and MIDI events
* presenting playback and recording state
* sending user intents to the native core
* reacting to notifications and state changes coming from the core

The UI runs inside a **Saucer-hosted WebView** and is implemented using:

* **HTML**
* **CSS**
* **TypeScript**
* optionally **Lit**
* optionally **Bootstrap**
* bundled into JavaScript for runtime use

---

# 2. UI Design Goals

## 2.1 Main goals

The UI shall be:

* productive to develop
* easy to maintain
* responsive during playback and editing
* suitable for desktop workflows
* visually clear for music editing
* synchronized with the native core
* modular enough to support future growth

## 2.2 Secondary goals

The UI should also be:

* easy to debug in browser dev tools
* mostly component-based
* state-driven rather than DOM-manipulation-driven
* keyboard-friendly
* prepared for multiple open documents/windows

---

# 3. Technology Recommendations

## 3.1 Baseline stack

Recommended MVP stack:

* **TypeScript**
* **Lit** for components
* **Bootstrap** for layout, spacing, forms, and common controls
* **Custom CSS** for music-specific views
* **Parcel** for build/dev bundling

## 3.2 Why this stack

This gives:

* fast development
* strongly typed code
* good component structure
* low setup burden
* flexibility for a desktop app UI

## 3.3 Rendering recommendation for score

For the **music score view**, the best MVP recommendation remains:

* **HTML for layout and panels**
* **SVG for score graphics and symbols**

Not pure canvas as the initial default.

Why:

* easier hit testing
* easier CSS styling
* easier DOM inspection
* easier incremental updates
* easier handling of ties, rests, clefs, and symbols than pure HTML alone
* lower complexity than a full custom canvas engine

Canvas can still be introduced later for denser editors or performance-heavy views.

---

# 4. High-Level UI Architecture

The UI should be structured into these layers:

## 4.1 Application shell layer

Responsible for:

* bootstrapping the UI app
* wiring the bridge to native core
* global app state
* document/window initialization
* routing top-level commands

## 4.2 State layer

Responsible for:

* local UI state
* mirrored core state
* derived view state
* selection state
* active tool/mode
* layout preferences

## 4.3 View/component layer

Responsible for rendering:

* menu-related command surfaces
* document tabs or document views
* score area
* mixer
* MIDI event editor
* transport controls
* dialogs/panels

## 4.4 Bridge/service layer

Responsible for:

* sending commands to core
* receiving responses and events
* updating local state
* handling resync/snapshots

---

# 5. UI Source Structure

Recommended `ui/src` layout:

```text
ui/src/
  app/
    main.ts
    bootstrap.ts
    appShell.ts
    store.ts

  bridge/
    coreBridge.ts
    messageTypes.ts
    messageParser.ts
    eventDispatcher.ts

  models/
    document.ts
    track.ts
    note.ts
    measure.ts
    transport.ts
    selection.ts
    layout.ts
    preferences.ts

  services/
    commandService.ts
    documentService.ts
    selectionService.ts
    scoreLayoutService.ts
    notationService.ts
    clipboardService.ts
    preferencesService.ts

  components/
    shell/
      app-root.ts
      document-tabs.ts
      status-bar.ts

    transport/
      transport-bar.ts
      play-button.ts
      stop-button.ts
      record-button.ts
      tempo-display.ts

    layout/
      split-pane.ts
      collapsible-panel.ts

    score/
      score-view.ts
      score-toolbar.ts
      score-header.ts
      score-track-lane.ts
      score-measure.ts
      note-glyph.ts
      rest-glyph.ts
      clef-glyph.ts
      tie-glyph.ts
      accidental-glyph.ts
      measure-grid.ts

    mixer/
      mixer-panel.ts
      mixer-channel-strip.ts
      master-strip.ts

    midi-events/
      midi-events-panel.ts
      midi-events-table.ts
      midi-event-editor.ts

    dialogs/
      preferences-dialog.ts
      open-recent-dialog.ts
      about-dialog.ts
      confirm-dialog.ts

  styles/
    main.css
    theme.css
    layout.css
    score.css
    mixer.css
    midi-events.css

  assets/
    icons/
    music/
      clefs/
      rests/
      notes/
      accidentals/
```

---

# 6. Main UI Layout

## 6.1 Main window model

The application must support multiple opened MIDI documents.

There are two acceptable UX models:

* one native window hosting one document
* one native window hosting multiple document tabs

Because your requirement says *the main window could contain multiple opened MIDIs*, the UI architecture should support a **tabbed document area**.

For MVP, recommended behavior is:

* support multiple open documents in app state
* allow tabbed document switching in the main UI
* keep the bridge and core document-aware

## 6.2 Main window sections

The main window UI contains:

* document tab area
* transport controls
* three collapsible working areas:

  * music score view
  * mixer
  * MIDI events panel
* optional status bar

## 6.3 Recommended arrangement

Recommended default layout:

* **top**: document tabs + transport bar
* **center**: score view
* **right side**: mixer
* **bottom**: MIDI events panel

The mixer and MIDI events panel must be collapsible.

This is a strong desktop-oriented layout and maps well to your requirements.

---

# 7. Top-Level Screens and Panels

## 7.1 App shell

Responsibilities:

* host all major UI regions
* bind current active document
* manage top-level layout state
* route menu and toolbar actions

Contains:

* document tabs
* transport bar
* main content split layout
* dialogs
* status area

---

## 7.2 Document tabs

Needed because the main window may contain multiple opened MIDIs.

Tab responsibilities:

* show document title
* show unsaved/dirty marker
* switch active document
* close document
* possibly open document menu/context menu

Example tab label:

* `Song A`
* `Song B *`

Where `*` means unsaved changes.

---

## 7.3 Score view

This is the central editing view.

Responsibilities:

* display tracks vertically
* display measures horizontally
* display notes, rests, clefs, time signatures, tempo changes, ties, accidentals
* support edit mode and play mode
* support selection and note editing
* scroll horizontally and vertically
* show playback cursor/current position

This is the most important UI component.

---

## 7.4 Mixer

Responsibilities:

* one channel strip per track
* master strip
* per-track volume, pan, effects, mute, solo, arm
* transport shortcuts if desired
* visible state for current playback/recording status

Must be collapsible.

---

## 7.5 MIDI events panel

Responsibilities:

* show MIDI events in an intelligent tabular interface
* allow manual editing of event properties
* support event selection and filtering
* show controllers, pitch bends, program changes, note events as needed

Must not be “text only.”

The UX should feel like a structured table/editor, not raw event code editing.

Must be collapsible.

---

# 8. Menu Bar Specification

The native shell owns the actual desktop menu bar, but the UI must define the command model behind it.

## 8.1 File menu

Commands:

* New
* Open
* Open Recent
* Save
* Save As
* Import MIDI
* Export MIDI
* Close
* Exit

## 8.2 Edit menu

Commands:

* Undo
* Redo
* Cut
* Copy
* Paste
* Delete
* Select All
* Preferences shortcut if desired in native platform style

## 8.3 Preferences menu

Commands/dialogs for:

* general settings
* MIDI input/output selection
* metronome settings
* theme/display settings
* score rendering settings
* grid/snap settings
* playback/recording settings

## 8.4 Window menu

Commands:

* list open documents/windows
* switch active document
* organize windows
* maybe reset layout
* toggle panels

## 8.5 Help menu

Commands:

* contents
* search
* shortcuts
* about
* maybe diagnostics/log view later

---

# 9. UI State Model

The UI should distinguish between:

## 9.1 Core-owned mirrored state

Mirrored from core, authoritative in core:

* document structure
* tracks
* notes
* measures derived from snapshot or calculated locally
* tempo/time signature events
* transport state
* playback position
* device lists
* dirty/revision state

## 9.2 UI-only state

Owned purely in frontend:

* current selection
* active tool
* zoom
* scroll position
* collapsed/expanded panels
* active tab within MIDI events panel
* hover state
* drag state
* local editing overlays
* edit mode vs play mode display state

This separation is important.

---

# 10. Document View Model

Each open document in the UI should have a view model like this:

```ts
interface DocumentViewModel {
  id: string;
  title: string;
  revision: number;
  dirty: boolean;
  ppqn: number;

  tracks: TrackViewModel[];
  tempoMap: TempoEventViewModel[];
  timeSignatureMap: TimeSignatureEventViewModel[];

  transport: TransportViewModel;
  layout: DocumentLayoutState;
  selection: SelectionState;
  scoreMode: 'edit' | 'play';
}
```

This does not replace the core model; it mirrors what the UI needs.

---

# 11. Score View Specification

This deserves the most detail.

## 11.1 Responsibilities

The score view shall:

* show all visible tracks in vertical lanes
* show measures horizontally
* render notes using musical notation rules appropriate for the current supported scope
* show clef, time signature, and tempo marking at the beginning of a track or where changes occur
* render ties across measure boundaries correctly
* render rests automatically where there are no active notes
* support scrolling horizontally and vertically
* support note editing tools
* support copy, paste, cut, and delete interactions
* show playback cursor in play mode
* support add track and add measure actions

---

## 11.2 Score modes

The score view has two primary modes:

### Edit mode

Used for:

* adding notes
* editing durations
* moving notes
* inserting accidentals
* selecting notes
* deleting/copying/cutting/pasting notes

### Play mode

Used for:

* following playback
* reduced editing affordances
* clearer playback cursor display
* maybe note highlighting

Mode should be visually clear.

---

## 11.3 Score structure

Recommended conceptual structure:

```text
ScoreView
  ├── ScoreToolbar
  ├── ScoreHeaderRow
  ├── Horizontal scroll container
  │     ├── Vertical track lanes
  │     │     ├── Track header / instrument label
  │     │     ├── Measure 1
  │     │     ├── Measure 2
  │     │     ├── ...
  │
  └── overlays
        ├── playback cursor
        ├── selection highlight
        ├── drag preview
        ├── insertion preview
```

---

# 12. Measure and Track Rendering Rules

## 12.1 Clef, time signature, and tempo

At the start of a track, the UI shall display:

* clef
* current time signature
* current tempo/play speed if that is part of the visible notation policy

If any of these change at a later measure, they shall be shown again at the change point.

This means the score renderer must be aware of:

* the effective value at measure start
* changes occurring within the measure or at measure boundaries

## 12.2 Track lanes

Each track lane should include:

* track name
* instrument/channel info, optional compact display
* measure content cells
* optional quick controls at the lane start

Tracks scroll vertically.

## 12.3 Measure layout

Each measure should know:

* start tick
* end tick
* effective time signature
* effective tempo at start
* visible symbols to show at start
* contained notes and rests
* tie continuation in/out markers

---

# 13. Note Rendering Rules

## 13.1 General rule

Notes are rendered according to:

* start tick
* duration
* effective measure boundaries
* time signature
* notation grouping rules
* accidental context where supported

## 13.2 Cross-measure notes and ties

You explicitly required correct tie rendering across measures.

The UI shall:

* detect when a note duration crosses a measure boundary
* split the visual representation into measure-local note segments
* render ties connecting the note heads across measure boundaries
* continue this across as many measures as necessary until the total duration completes

Important:

* this is a **visual notation split**, not a destructive edit to the core note unless the user explicitly performs that action
* the original core note can remain a single note entity

This means the UI needs a **notation layout service** that derives visual note fragments from canonical notes.

## 13.3 Eighth and sixteenth notes

The score shall render eighth and sixteenth notes correctly according to:

* note duration in ticks
* current PPQN
* effective time signature
* measure grouping rules

This requires duration-to-notation conversion logic, not just proportional drawing.

## 13.4 Accidentals

Edit tools should allow insertion or adjustment of accidentals where applicable in the notation model.
Even if full notation semantics are not complete in MVP, the architecture should reserve support for:

* sharp
* flat
* natural

---

# 14. Rest Rendering Rules

You explicitly required pauses to be automatically calculated and drawn.

## 14.1 Rule

If no note is active during part of a measure, the UI should derive and render rests for the silent portions.

## 14.2 Important architectural note

Rests do not need to exist as first-class stored core entities for MVP.

Instead:

* the UI computes them from note occupancy within the measure
* the notation service generates appropriate rest symbols/fragments

## 14.3 Rest computation behavior

For a given track and measure:

* gather notes intersecting the measure
* compute occupied time ranges
* compute gaps
* convert gaps into rest notation fragments

This is a derived layout concern, not raw persistence data.

---

# 15. Score Editing Tools

## 15.1 Required tools

In edit mode, the score shall support tools for:

* inserting notes
* changing note duration
* moving notes in time
* selecting notes
* deleting notes
* copy
* cut
* paste
* adding/editing accidentals
* possibly split/merge later

## 15.2 Tool model

Recommended tool enum:

```ts
type ScoreTool =
  | 'select'
  | 'insert-note'
  | 'resize-note'
  | 'erase'
  | 'accidental-sharp'
  | 'accidental-flat'
  | 'accidental-natural';
```

You can extend later.

## 15.3 Interaction rule

The UI may minimally preprocess user interaction before sending to core.

Example:

* click on measure/staff position
* UI resolves intended note pitch, track, start tick, and default duration
* UI sends `CreateNoteCommand` to core

That matches your requirement.

---

# 16. Copy, Cut, Paste, Delete Behavior

These operations should work from score selection.

## 16.1 Copy

Copies selected note/event data into a UI clipboard structure.

## 16.2 Cut

Copies selection and requests deletion from core.

## 16.3 Paste

Inserts clipboard contents at current insertion point or selection anchor.

## 16.4 Delete

Deletes selected notes/events.

The UI should prefer sending structured commands to core rather than trying to mutate local mirrored state directly.

---

# 17. Score Scrolling and Navigation

## 17.1 Horizontal scrolling

The score view must scroll horizontally to reveal additional measures.

## 17.2 Vertical scrolling

The score view must scroll vertically to reveal additional tracks.

## 17.3 Playback following

In play mode, optionally:

* auto-scroll horizontally to keep playback cursor visible
* maybe keep active track region visible if configured

## 17.4 Zoom

Even though you did not explicitly require it, score zoom is worth adding to the UI spec now.

Recommended support:

* horizontal zoom
* notation scaling
* maybe independent staff/row height later

---

# 18. Add Track and Add Measure UX

You suggested a `(+)` button at the top or right. That is a good fit.

## 18.1 Add track

Recommended UI:

* plus button near track header column
* or context action in track list/header area

Behavior:

* opens quick add track flow
* creates track with default name and defaults
* updates mixer and score immediately

## 18.2 Add measure

Recommended UI:

* plus button at far right of visible measures
* or command in toolbar/context menu

Behavior:

* extends composition structure logically through timeline editing
* creates additional visible measure space

Architecturally, measures remain timeline-derived, but the UI can expose a simple “extend composition” action.

---

# 19. Mixer Specification

## 19.1 Responsibilities

The mixer shall show:

* one strip per track
* master strip

Each track strip should include at minimum:

* track name
* volume
* pan
* mute
* solo
* arm
* maybe effects send values for MVP if supported

Master strip should include:

* main volume
* play
* stop
* record shortcuts if desired there
* maybe metronome toggle

## 19.2 Mixer interactions

UI changes in the mixer send commands to core such as:

* `SetTrackVolumeCommand`
* `SetTrackPanCommand`
* `SetTrackMuteCommand`
* `SetTrackSoloCommand`
* `SetTrackArmCommand`

## 19.3 Visual state

Mixer should visibly reflect:

* mute/solo/arm state
* track currently selected
* active playback/recording state

---

# 20. MIDI Events Panel Specification

## 20.1 Goal

The MIDI events panel should allow manual event editing without forcing the user into a raw text editor.

## 20.2 Recommended form

Use a structured **table/grid interface** with typed cells.

Columns can include:

* event type
* tick/time
* track
* channel
* value fields depending on event type

## 20.3 Supported event categories

For MVP, the panel should support viewing/editing:

* notes
* control changes
* pitch bends
* program changes
* maybe tempo/time signature events in a separate subview

## 20.4 Smart editing behavior

Instead of raw event text, use:

* dropdowns for event types
* numeric inputs/sliders for values
* contextual editors depending on event type
* sorting/filtering
* maybe grouping by measure

This satisfies your “intelligent interface, not text only” requirement.

---

# 21. Transport Bar Specification

Even though transport was mentioned inside the mixer, it is worth giving it a clear dedicated component.

## 21.1 Controls

At minimum:

* Play
* Stop
* Record
* current position display
* tempo display
* metronome toggle
* loop toggle later if desired

## 21.2 State display

Show:

* playback state
* recording state
* current tick/measure-beat location

## 21.3 Source of truth

Transport state comes from core notifications.

---

# 22. Notifications and Synchronization

## 22.1 Required behavior

The UI shall:

* receive notifications from the core during playback
* receive note/model updates from the core
* update visible state incrementally
* remain synced with the authoritative native state

## 22.2 Core event handling

Important UI events include:

* document snapshot received
* note created/updated/deleted
* track updated
* transport state changed
* playback position changed
* recording started/stopped

## 22.3 Local update strategy

Recommended:

* maintain local mirrored store
* apply typed updates from bridge events
* re-render affected components only

---

# 23. UI Component Model

Recommended top-level Lit components:

## 23.1 Main components

* `<mc-app-root>`
* `<mc-document-tabs>`
* `<mc-transport-bar>`
* `<mc-score-view>`
* `<mc-score-toolbar>`
* `<mc-mixer-panel>`
* `<mc-midi-events-panel>`
* `<mc-status-bar>`

## 23.2 Score subcomponents

* `<mc-score-track-lane>`
* `<mc-score-measure>`
* `<mc-note-glyph>`
* `<mc-rest-glyph>`
* `<mc-clef-glyph>`
* `<mc-tie-glyph>`
* `<mc-playback-cursor>`

These can render SVG internally.

---

# 24. Rendering Architecture for Score Symbols

## 24.1 Preferred strategy

Use **SVG-based rendering** for notation symbols.

Possible approaches:

### A. SVG assets

Store clefs, rests, noteheads, accidentals as SVG files.

### B. Programmatic SVG components

Generate SVG in code for some glyphs and ties.

### C. Hybrid

Use static SVG assets for symbols and programmatic SVG for layout-specific shapes like ties and beams.

This hybrid is the best recommendation.

## 24.2 PNG recommendation

PNG should not be the main solution for note rendering.
It is weaker for scaling and theming.

---

# 25. Notation Layout Service

Because score rendering is not a direct 1:1 mapping from raw notes, the UI needs a notation/layout service.

## 25.1 Responsibilities

The notation service shall:

* map notes to measure-local visual fragments
* split cross-measure durations for tie rendering
* compute rests from silent gaps
* determine note symbols from tick durations
* decide where clefs/time signatures/tempo changes must be shown
* prepare visual layout data for rendering components

## 25.2 Suggested outputs

For each visible measure and track, derive:

* visible symbols
* note fragments
* rest fragments
* tie start/end/continuation
* change markers
* spacing metadata

This service is a key piece of the frontend architecture.

---

# 26. Preferences and Configurable UI Behavior

The UI should support preferences for at least:

* theme
* score zoom
* default note duration
* snap/grid settings
* MIDI device preferences view
* metronome settings
* auto-scroll during playback
* show/hide mixer by default
* show/hide MIDI events by default

---

# 27. Keyboard and Desktop Interaction

This is worth defining now.

## 27.1 Basic shortcuts

Expected shortcuts:

* Ctrl/Cmd+N — new
* Ctrl/Cmd+O — open
* Ctrl/Cmd+S — save
* Ctrl/Cmd+Z — undo
* Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y — redo
* Delete — delete selection
* Ctrl/Cmd+C — copy
* Ctrl/Cmd+X — cut
* Ctrl/Cmd+V — paste
* Space — play/stop, if desired
* R — record, if desired

## 27.2 Desktop interaction expectations

* right-click context menus in score and MIDI events panel
* double-click useful for note edit actions
* drag interactions for moving/resizing notes later if desired

---

# 28. Responsive Behavior

This is a desktop-first UI, but layout should still adapt to window resizing.

## 28.1 Requirements

* score remains central and resizable
* mixer can collapse when space is small
* MIDI events panel can collapse when space is small
* tabs remain usable with many documents open

---

# 29. Error and Empty States

The UI should define clear states for:

* no document open
* loading/opening document
* empty composition
* no MIDI devices available
* invalid command result
* failed import/export
* desync requiring resnapshot

These often get forgotten and then hurt UX.

---

# 30. Recommended Initial UI Decisions

To keep things practical, I’d lock these in now:

* **UI stack:** TypeScript + Lit + Bootstrap
* **Score rendering:** SVG inside HTML layout
* **Main layout:** tabs + transport bar + score center + mixer right + MIDI events bottom
* **Three work areas:** all collapsible
* **State model:** mirrored core state + UI-only interaction state
* **Synchronization:** snapshot + typed incremental events
* **Note/tie/rest rendering:** handled by a notation layout service in UI
* **Track and measure add actions:** plus buttons in score header regions
* **MIDI events editing:** intelligent grid/table, not raw text

---

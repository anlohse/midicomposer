# MIDI Composer — Implementation Phases Document

## 1. Purpose

This document defines the implementation roadmap for the MIDI Composer project in a sequence of **small, verifiable phases**.

Each phase must end with:

* a buildable project
* a runnable application
* a concrete verification checklist
* a clear set of completed capabilities

The goal is to ensure that progress is always visible and testable, instead of accumulating too much unfinished infrastructure before seeing results.

---

# 2. Implementation Strategy

## 2.1 Guiding principles

The project should be built in **vertical slices**, not only by technical layers.

That means each phase should try to include, when possible:

* some native core work
* some UI work
* some bridge work
* a runnable result

## 2.2 Phase completion rule

A phase is only considered complete when:

* the code compiles
* the app runs
* the expected behavior can be manually verified
* the phase exit checklist passes

## 2.3 MVP planning rule

The roadmap should prioritize:

* foundation first
* visible end-to-end behavior early
* core correctness before advanced notation polish
* editing/playback basics before advanced music theory features

---

# 3. Phase Overview

Recommended implementation phases:

1. Project setup and build integration
2. Minimum app shell, bridge, and basic menus
3. Document model, open/new/save shell, and first document UI
4. Minimum score view shell and layout
5. Core musical model and basic note editing
6. Score rendering MVP: measures, notes, rests, ties
7. Mixer MVP
8. MIDI events panel MVP
9. Transport and playback MVP
10. Recording and metronome MVP
11. MIDI import/export MVP
12. Native project persistence and polish
13. Stabilization and MVP hardening
14. Distribution and SmartScreen — post-MVP, required for 1.0.0 (§18)

---

# 4. Phase 1 — Project Setup and Build Integration

## 4.1 Goal

Create the repository structure, configure native and UI build systems, and make the app launch with Saucer and a WebView-hosted UI.

## 4.2 Scope

### Core

* create `core/` structure
* create `core/src/`
* create initial `CMakeLists.txt`
* configure C++20
* configure warnings/options
* add dependency strategy
* integrate Saucer
* integrate chosen header-only logger
* prepare dependency helper CMake files

### UI

* create `ui/` structure
* create `ui/src/`
* create `package.json`
* create `tsconfig.json`
* add Parcel setup
* add Lit and Bootstrap dependencies
* create minimal frontend entrypoint

### Integration

* make native app open a Saucer window
* load UI page in development mode
* confirm UI is visible inside native window

## 4.3 Deliverables

* repository layout created
* root `.gitignore`
* `ui/.gitignore`
* native build succeeds
* UI build succeeds
* native app launches and displays basic frontend page

## 4.4 Manual verification checklist

When this phase is done, you should be able to:

* configure the native build with CMake
* build the native executable
* run the UI dev server
* launch the native app
* see a basic page inside the Saucer window, such as:

  * app title
  * placeholder text like “MIDI Composer UI Loaded”

## 4.5 Exit criteria

Phase 1 is complete when:

* app builds cleanly
* UI builds cleanly
* native window opens
* WebView loads the UI successfully

---

# 5. Phase 2 — Minimum App Shell, Bridge, and Basic Menus

## 5.1 Goal

Create the first real end-to-end application shell with a working UI ↔ core bridge and a few simple menu actions.

This matches your example very well.

## 5.2 Scope

### Core

* implement minimal app shell classes
* implement minimal `CoreFacade`
* implement JSON bridge skeleton
* support a ping/test command
* support app-level actions such as exit
* prepare help/about wiring

### UI

* create basic application shell component
* create placeholder main layout
* create bridge service in TypeScript
* create menu command display or action hooks where relevant
* add About dialog UI
* add Help content viewer support for Markdown files or a simple help panel

### Menus

Implement at least:

* File → Exit
* Help → About
* Help → Contents

## 5.3 Deliverables

* bridge request/response path works
* About dialog works
* Help content can be shown
* File → Exit closes the application

## 5.4 Manual verification checklist

You should be able to:

* launch the app
* confirm UI says it is connected to core
* click Help → About and see dialog
* click Help → Contents and see help content
* click File → Exit and close the app cleanly

## 5.5 Exit criteria

Phase 2 is complete when:

* core and UI can exchange JSON messages
* at least one UI command goes to core and returns a result
* simple menu actions are functional
* the app exits cleanly through the menu

---

# 6. Phase 3 — Document Lifecycle and First Real Document Screen

## 6.1 Goal

Introduce document management and the first real project/document state.

## 6.2 Scope

### Core

* implement `ProjectDocument`
* implement `DocumentManager`
* support:

  * new project
  * close project
  * active document tracking
* return a minimal document snapshot through the bridge

### UI

* support multiple open documents in state
* create document tabs
* render active document title
* show dirty flag placeholder behavior
* display empty document workspace

### Menus

Implement:

* File → New
* File → Close
* Window menu listing open documents, if practical at this stage

## 6.3 Deliverables

* opening a new document creates a visible new tab or document view
* active document can switch
* document snapshot can be requested and rendered

## 6.4 Manual verification checklist

You should be able to:

* click File → New
* see a new document appear
* create multiple documents
* switch between them
* close one document
* see the active document update correctly

## 6.5 Exit criteria

Phase 3 is complete when:

* documents exist as real app state
* multiple documents can be opened in the UI
* the UI renders an active document view based on core state

---

# 7. Phase 4 — Main Window Layout and Empty Editor Shell

## 7.1 Goal

Build the real desktop editing layout, even if the musical content is still placeholder.

## 7.2 Scope

### UI

* implement main document workspace with:

  * score area
  * mixer area
  * MIDI events area
* make the three panels collapsible
* add transport bar placeholder
* add score toolbar placeholder
* support horizontal and vertical scrolling containers in score area

### Core / bridge

* provide enough document metadata to populate the layout
* add layout-related commands only if needed

## 7.3 Deliverables

* full editor shell visible
* collapsible score/mixer/MIDI events areas
* tabs and transport bar present
* empty document renders as a usable workspace

## 7.4 Manual verification checklist

You should be able to:

* open a document
* see the score area
* collapse and expand mixer
* collapse and expand MIDI events panel
* switch between document tabs
* see all panels update with active document

## 7.5 Exit criteria

Phase 4 is complete when:

* the real editor layout exists
* the application visually resembles the intended product shell
* panel layout behavior works reliably

---

# 8. Phase 5 — Core Musical Model and Basic Note Editing

## 8.1 Goal

Implement the first real musical data model and allow basic note creation/editing through the score view.

## 8.2 Scope

### Core

* implement:

  * `Composition`
  * `Track`
  * `Note`
  * tempo map and time signature map basics
* support:

  * create track
  * create note
  * move note
  * resize note
  * delete note
* add revision tracking
* emit note change notifications

### UI

* create score interaction basics
* implement:

  * select tool
  * insert-note tool
  * erase tool
* render a simple temporary note representation, even before full notation
* react to note-created and note-updated events

### Menus / commands

* Edit → Undo/Redo may still be placeholder unless command stack is ready
* Edit → Cut/Copy/Paste/Delete hooks begin to exist

## 8.3 Deliverables

* a document can contain tracks and notes
* a user click can create a note
* notes can be selected and deleted
* notes can be moved/resized at a basic level

## 8.4 Manual verification checklist

You should be able to:

* create a new document
* add a track
* click in the score to insert notes
* select a note
* delete a note
* drag a note to another time position
* resize a note duration

## 8.5 Exit criteria

Phase 5 is complete when:

* note editing works end-to-end from UI gesture to core update and back to UI
* document revisions update correctly
* note changes are reflected immediately in the score view

---

# 9. Phase 6 — Score Rendering MVP

## 9.1 Goal

Replace temporary note rendering with real notation-oriented rendering.

## 9.2 Scope

### UI

* implement notation rendering service
* render:

  * measures
  * clefs
  * time signatures
  * tempo markers at supported locations
  * note values
  * eighth and sixteenth notes
  * ties across measures
  * automatically derived rests
* support horizontal scrolling across measures
* support vertical scrolling across tracks

### Core

* provide the data needed by renderer:

  * PPQN
  * time signatures
  * tempo map
  * tracks and notes

## 9.3 Deliverables

* score looks like real notation instead of placeholders
* cross-measure ties render correctly
* rests are shown automatically in silent spans
* time signature and clef display rules work at track starts and change points

## 9.4 Manual verification checklist

You should be able to:

* create quarter, eighth, and sixteenth notes and see them rendered correctly
* create a note that crosses a measure boundary and see tied fragments
* see rests appear where no note is present
* see clef/time signature at the beginning of the track
* see time signature or tempo changes shown again when changed later

## 9.5 Exit criteria

Phase 6 is complete when:

* score rendering is musically coherent for MVP
* notation rules for ties, rests, and supported durations work visibly

---

# 10. Phase 7 — Mixer MVP

## 10.1 Goal

Introduce a functional mixer panel tied to track state.

## 10.2 Scope

### Core

* track parameters:

  * volume
  * pan
  * mute
  * solo
  * arm
* update commands and notifications

### UI

* render mixer strips per track
* render master strip
* allow editing the supported track controls

## 10.3 Deliverables

* mixer panel reflects track list
* per-track controls update the core
* core changes update the UI

## 10.4 Manual verification checklist

You should be able to:

* create multiple tracks
* see one mixer strip per track
* change volume and pan
* toggle mute/solo/arm
* see state stay synchronized with the core

## 10.5 Exit criteria

Phase 7 is complete when:

* mixer is functional for basic track control
* mixer state and document state remain synchronized

---

# 11. Phase 8 — MIDI Events Panel MVP

## 11.1 Goal

Implement the MIDI events panel as a structured event editor.

## 11.2 Scope

### Core

* expose note events, controller events, pitch bends, program changes as needed
* add edit/query commands for MIDI events

### UI

* implement MIDI event grid/table
* allow selection/filtering
* allow editing supported fields through structured controls

## 11.3 Deliverables

* MIDI events panel displays relevant events
* user can edit at least some event types manually
* panel is not raw text

## 11.4 Manual verification checklist

You should be able to:

* select a track or measure scope
* see related note/controller events in the panel
* edit an event value in the table
* see the score or internal state update accordingly

## 11.5 Exit criteria

Phase 8 is complete when:

* the MIDI events panel is useful for inspection and basic editing
* edits round-trip correctly through the core

---

# 12. Phase 9 — Transport and Playback MVP

## 12.1 Goal

Implement real playback through MIDI output and transport synchronization with the UI.

## 12.2 Scope

### Core

* transport state machine
* play
* stop
* pause
* seek
* playback scheduler
* playback notifications to UI
* MIDI output routing

### UI

* transport bar becomes functional
* playback position cursor in score
* optional auto-scroll follow
* play mode behavior

## 12.3 Deliverables

* composition plays to MIDI output
* UI receives playback updates
* score shows moving playback cursor

## 12.4 Manual verification checklist

You should be able to:

* press Play
* hear MIDI output
* see transport state change
* see playback cursor move across the score
* press Stop
* see playback stop cleanly
* click to seek and play from another position

## 12.5 Exit criteria

Phase 9 is complete when:

* playback works end-to-end
* UI transport stays synchronized with core transport

---

gemini --resume 'b561822e-b7ce-4e30-8008-c4b1bf077a52'

# 13. Phase 10 — Recording and Metronome MVP

## 13.1 Goal

Allow recording notes from MIDI input with metronome support.

## 13.2 Scope

### Core

* MIDI input device support
* recording engine
* note-on/note-off pairing
* arm track support
* metronome
* count-in support if feasible in this phase

### UI

* record button functional
* track arm control integrated with recording
* metronome toggle
* recording state notifications

## 13.3 Deliverables

* armed track can record notes from MIDI input
* recorded notes appear in document
* metronome works during recording

## 13.4 Manual verification checklist

You should be able to:

* choose a MIDI input device
* arm a track
* start recording
* play notes on a MIDI keyboard
* stop recording
* see recorded notes appear in the score
* hear/observe metronome behavior if enabled

## 13.5 Exit criteria

Phase 10 is complete when:

* basic recording is functional
* recorded notes become normal editable document notes
* metronome works for MVP recording workflow

---

# 14. Phase 11 — MIDI Import/Export MVP

## 14.1 Goal

Support loading and saving standard MIDI files.

## 14.2 Scope

### Core

* MIDI file parser
* MIDI file exporter
* import into canonical model
* export from canonical model

### UI

* File → Import MIDI
* File → Export MIDI
* display imported results in score/mixer/event panels

## 14.3 Deliverables

* import a MIDI file into a document
* export current document to MIDI file

## 14.4 Manual verification checklist

You should be able to:

* import a MIDI file
* see tracks and notes populate
* play imported content
* export the document
* reopen the exported MIDI and confirm basic fidelity

## 14.5 Exit criteria

Phase 11 is complete when:

* MIDI import/export is usable for normal workflows
* imported material is editable and playable

---

# 15. Phase 12 — Native Project Persistence

## 15.1 Goal

Implement the app’s native editable project format.

## 15.2 Scope

### Core

* JSON-based native project serialization/deserialization
* schema versioning
* save/load project document
* preserve editor-relevant metadata

### UI

* File → Save
* File → Save As
* File → Open
* dirty state integration
* recent files support can begin here or later

## 15.3 Deliverables

* native project format works
* project reload preserves app-specific structure better than MIDI alone

## 15.4 Manual verification checklist

You should be able to:

* create a project
* add tracks and notes
* save in native format
* close and reopen
* confirm notes, tracks, and relevant metadata are preserved
* see dirty state behave correctly

## 15.5 Exit criteria

Phase 12 is complete when:

* native project save/load works reliably
* dirty-state and basic file lifecycle are correct

---

# 16. Phase 13 — Stabilization and MVP Hardening

## 16.1 Goal

Make the MVP reliable, consistent, and demo-ready.

## 16.2 Scope

### Core

* bug fixing
* validation hardening
* better error messages
* cleanup of threading and resource handling
* polish on undo/redo if already present

### UI

* visual cleanup
* keyboard shortcut polish
* better empty states
* error dialogs/messages
* panel layout persistence if feasible
* Help content cleanup

### Build/package

* development and production load modes validated
* UI asset packaging cleanup
* release build sanity checks

## 16.3 Deliverables

* stable MVP
* consistent user flow
* reduced crashes/desyncs
* basic packaging readiness

## 16.4 Manual verification checklist

You should be able to:

* create/open/save/import/export reliably
* edit notes
* play and record
* use mixer and MIDI events panel
* switch documents
* close the app cleanly without corrupting work

## 16.5 Exit criteria

Phase 13 is complete when:

* the app is usable as an MVP
* the main workflows are stable enough for regular testing/demo use

---

# 18. Phase 14 — Distribution and SmartScreen

Post-MVP, and a requirement for 1.0.0 — possibly worth its own 0.4.0.

## 18.1 The problem

The installer is unsigned. Windows SmartScreen shows "Windows protected your
PC" for a file that carries the Mark of the Web and has no reputation, and an
unsigned installer never acquires one. A person downloading MIDI Composer has to
click through a warning that, read plainly, tells them not to.

Measured on 2026-09-02, so the starting point is not in doubt:

* the installer target builds — `MIDIComposer-0.3.0-setup.exe`, 758 KB, from the
  hand-written `installer/midi_composer.nsi` via NSIS
* `Get-AuthenticodeSignature` reports **NotSigned**

That is the whole of it. Nothing is broken; nothing is signed.

## 18.2 The two routes

Researched by the project owner:

| Route | Cost | Availability | SmartScreen | Store eligible | Suits |
|---|---|---|---|---|---|
| Microsoft Store (MSIX) — the Store re-signs the package | Free | Worldwide | No warnings | Yes | Recommended for most new applications |
| Microsoft Store (MSI/EXE installer) — the publisher signs | Certificate chaining to a Trusted Root Program CA, price varies by CA | Worldwide | No SmartScreen prompt when installed from the Store (UAC may still appear) | Yes | Existing Win32 applications shipped through the MSI/EXE path |

Two things to confirm against current Microsoft policy rather than take from
here, because both have moved before: whether a standard (OV) code signing
certificate now earns SmartScreen reputation immediately or still accrues it
over downloads, and what a Partner Center developer account costs today.

## 18.3 What either route costs this application

Not a packaging checkbox. Three parts of the design touch it:

* **The WebView2 runtime.** The NSIS script detects it and bootstraps it at
  install time, which is why the installer is hand-written rather than CPack's.
  An MSIX declares dependencies instead of running code, so this has to be
  expressed as a package dependency or the runtime has to be carried.
* **The plugin folder.** §10.3 gave the application a folder to paste CLAP
  plugins into, deliberately user-writable. MSIX virtualises parts of the
  filesystem, and loading native code a user dropped into a folder is exactly
  the kind of thing a packaged app is restricted from doing freely. This needs
  answering before choosing MSIX, not after.
* **Preferences, the log and the webview profile.** They live under `%APPDATA%`
  and `%LOCALAPPDATA%` (see `app/preferences.cpp` and `base/logger.hpp`). Under
  MSIX those writes are redirected, which is survivable but changes where a
  person finds their own files — and the crash log is only useful if it can be
  found.

## 18.4 Order of work

Signing the existing installer is the smaller change and does not disturb the
plugin story, so it is the first thing to price. The Store MSIX route is the
better end state if the plugin folder question resolves, and the answer to that
is a decision about what the application is rather than about packaging.

---

# 17. Optional Cross-Phase Milestones

These are helpful checkpoints across the roadmap.

## 17.1 First visible milestone

After Phase 2:

* the app feels real because shell, menus, bridge, and dialogs exist

## 17.2 First editing milestone

After Phase 5:

* the user can actually create and edit notes

## 17.3 First musical notation milestone

After Phase 6:

* the score begins to look musically meaningful

## 17.4 First DAW-like milestone

After Phase 9:

* transport and playback are real

## 17.5 First creator workflow milestone

After Phase 10 or 11:

* user can compose, play, record, and exchange MIDI

---

# 18. Recommended Phase Size Rule

To keep phases healthy:

* each phase should be finishable in a reasonable span
* each phase must produce a runnable result
* avoid phases that are only “internal refactoring” unless they are very small or paired with visible progress

A good rule is:
**if a phase ends and you cannot demonstrate something concrete in the app, the phase is too abstract.**

---

# 19. Suggested Phase 1 and 2 Breakdown in Your Preferred Style

Here is the same plan in the style you described.

## 1. Setup Core, Setup UI

### a. Setup core CMake configurations and 3rd party dependencies

### b. Setup UI package, TypeScript, Parcel, Lit, Bootstrap

### c. Compile and run integrating core and UI

**Result to verify:**

* native window opens
* UI appears in Saucer
* build works from clean checkout

---

## 2. Minimum Core, Bridge, and Basic UI Shell

### a. Implement minimal core façade

### b. Implement minimum JSON communication bridge

### c. Implement basic UI shell

### d. Implement basic menu actions:

* File → Exit
* Help → Contents
* Help → About

### e. Compile and run integrating core and UI

**Result to verify:**

* UI talks to core
* About dialog works
* Help content opens
* Exit menu closes app

That structure works very well, and the rest of the phases can follow the same pattern.

---

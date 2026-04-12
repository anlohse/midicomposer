# MIDI Composer — Core Architecture Document

## 1. Document Purpose

This document defines the architecture for the **MIDI Composer** application, including:

* native **core engine** in modern C++
* **WebView-based UI** using Saucer
* repository and folder structure
* build tooling for core and UI
* communication between native core and UI
* recommended libraries and framework choices
* initial non-functional requirements
* open technical decisions and recommended direction

This document is intended to guide implementation from project bootstrap through MVP.

---

# 2. High-Level Product Architecture

The application is a **desktop MIDI composition editor** with:

* a **native C++ core**
* a **web-based UI** rendered inside a native window via **Saucer**
* bidirectional communication between UI and core
* support for opening and editing multiple MIDI documents
* real-time playback, recording, and transport notifications

## 2.1 Main architectural layers

The application is divided into these layers:

### A. Native Shell

Creates application windows, hosts the WebView, wires application lifecycle, menus, and native integrations.

### B. Core Engine

Implements musical model, editing, MIDI import/export, playback, recording, transport, persistence, and notifications.

### C. UI Frontend

Implements the visual editor using HTML, CSS, and TypeScript, rendered inside Saucer.

### D. Bridge Layer

Defines the message protocol between the WebView frontend and the C++ core.

---

# 3. Architectural Goals

## 3.1 Primary goals

* modern C++20+ core
* explicit ownership and RAII
* low-latency MIDI playback and recording
* clear module boundaries
* UI productivity using web technologies
* portability across desktop platforms
* preference for **header-only libraries** when practical
* reproducible builds with CMake for core and Node-based tooling for UI
* clean project layout from day one

## 3.2 Secondary goals

* easy debugging
* strong typing in C++ and TypeScript
* future support for advanced notation and richer editing tools
* low coupling between UI and engine
* minimal friction for contributors

---

# 4. Technology Stack

## 4.1 Native core

* **Language:** C++20 minimum
* **Build:** CMake
* **Window/WebView host:** Saucer
* **Logging:** header-only logging library
* **Testing:** lightweight C++ unit testing library
* **Serialization:** JSON library, preferably header-only
* **MIDI I/O:** backend abstraction with platform-specific implementations
* **Concurrency:** standard C++ threading primitives where sufficient

## 4.2 UI frontend

* **Language:** TypeScript
* **Markup/Styling:** HTML + CSS
* **Component/rendering option:** Lit
* **CSS/UI framework:** Bootstrap or equivalent
* **Bundler/dev build:** Parcel is acceptable for MVP
* **Package manager:** npm or pnpm
* **Static typing:** strict TypeScript configuration

## 4.3 Recommended library bias

Because you want to prioritize header-only libraries, the project should prefer header-only options unless there is a strong reason not to.

Good candidates for this policy include:

* logging
* JSON
* CLI/config parsing if needed
* testing helpers
* utility helpers
* strong enum/reflection helpers if added carefully

This preference should not become dogma. For performance-critical or platform-specific subsystems, a compiled dependency may still be the better choice.

---

# 5. High-Level Runtime Architecture

## 5.1 Processes

For MVP, the application should be a **single desktop process** containing:

* native shell
* core engine
* WebView UI

The UI runs inside the WebView hosted by the same process.

## 5.2 Major runtime components

### Native application host

Responsible for:

* app startup/shutdown
* window creation
* menu bar
* WebView creation
* bridge wiring
* document/window coordination

### Core façade

A high-level C++ API that the native shell and bridge use to access the engine.

### Project/document manager

Maintains open documents and active windows.

### Playback engine

Schedules MIDI output events.

### Recording engine

Consumes MIDI input and converts it into composition edits.

### Notification dispatcher

Pushes structured updates from the core to the UI.

### Frontend application

Receives state snapshots and incremental updates, renders views, and sends user commands back to the core.

---

# 6. Repository Layout

A clean monorepo-style structure is recommended.

```text
midi-composer/
  .gitignore
  README.md

  core/
    CMakeLists.txt
    cmake/
    external/                 # optional for vendored small libs
    src/
      app/
      base/
      device/
      edit/
      io/
      metronome/
      midi/
      music/
      notify/
      persistence/
      playback/
      project/
      recording/
      timeline/
      ui_bridge/
      shell/

    include/                  # optional public headers if you separate them
    tests/
    resources/
      icons/
      fonts/
      web/                    # built UI assets copied here for packaging, optional

  ui/
    package.json
    tsconfig.json
    .gitignore
    src/
      app/
      bridge/
      components/
      layout/
      models/
      services/
      styles/
      views/
      assets/
    public/
    dist/                     # generated build output
```

## 6.1 Important requirement from your note

The **source code must be in a `src` folder inside the `core` folder**.

So the architecture formally adopts:

* `core/src/...`
* `ui/src/...`

---

# 7. Core Folder Architecture

## 7.1 Core structure

Recommended `core/src` layout:

```text
core/src/
  app/
    core_facade.hpp
    core_facade.cpp
    document_manager.hpp
    document_manager.cpp

  shell/
    application.hpp
    application.cpp
    main.cpp
    window_controller.hpp
    window_controller.cpp
    menu_builder.hpp
    menu_builder.cpp

  ui_bridge/
    bridge_protocol.hpp
    bridge_protocol.cpp
    bridge_dispatcher.hpp
    bridge_dispatcher.cpp
    dto/
      commands.hpp
      events.hpp
      snapshots.hpp

  base/
    error.hpp
    strong_id.hpp
    result.hpp
    logger.hpp
    assertions.hpp

  music/
    composition.hpp
    track.hpp
    note.hpp
    controller_event.hpp
    pitch_bend_event.hpp
    tempo_map.hpp
    time_signature_map.hpp

  timeline/
    tick.hpp
    measure_map.hpp
    time_converter.hpp
    transport_position.hpp

  project/
    project_document.hpp
    project_repository.hpp

  edit/
    edit_service.hpp
    command.hpp
    undo_stack.hpp
    commands/

  playback/
    playback_engine.hpp
    playback_scheduler.hpp
    playback_event_builder.hpp

  recording/
    recording_engine.hpp
    recording_buffer.hpp
    note_capture_state.hpp

  metronome/
    metronome_engine.hpp

  midi/
    midi_message.hpp
    midi_encoding.hpp
    midi_constants.hpp

  io/
    midi_importer.hpp
    midi_exporter.hpp
    midi_file_reader.hpp
    midi_file_writer.hpp

  persistence/
    project_serializer.hpp
    project_deserializer.hpp

  device/
    midi_input.hpp
    midi_output.hpp
    midi_device_manager.hpp
    backend/

  notify/
    notification.hpp
    notification_bus.hpp
```

---

# 8. UI Folder Architecture

Recommended `ui/src` layout:

```text
ui/src/
  app/
    main.ts
    router.ts
    store.ts

  bridge/
    coreBridge.ts
    messageTypes.ts
    eventBus.ts

  components/
    common/
    mixer/
    score/
    midi-events/
    transport/
    dialogs/

  views/
    main-window/
    document-window/
    preferences/
    about/

  layout/
    splitLayout.ts
    docking.ts

  models/
    document.ts
    track.ts
    note.ts
    transport.ts

  services/
    commandService.ts
    renderService.ts
    selectionService.ts
    preferencesService.ts

  styles/
    main.css
    theme.css
    layout.css

  assets/
    icons/
    notes/
```

---

# 9. Core Architectural Modules

## 9.1 `shell`

Native desktop host.

Responsibilities:

* start application
* create windows
* attach Saucer webviews
* build menu bar
* connect menu actions to core
* manage per-window UI bootstrapping

This module should not implement music logic.

## 9.2 `ui_bridge`

Bridge between C++ and the WebView UI.

Responsibilities:

* encode/decode messages
* dispatch UI commands into core operations
* push notifications and snapshots to UI
* enforce protocol versioning and validation

This module is critical because it defines the contract between TypeScript and C++.

## 9.3 `app`

Application façade and orchestration layer.

Responsibilities:

* expose high-level operations
* coordinate documents, playback, and services
* act as the stable API used by shell and bridge

## 9.4 `music`

Canonical domain model.

Responsibilities:

* composition
* tracks
* notes
* controller events
* pitch bends
* tempo map
* time signatures

## 9.5 `timeline`

Musical time and measure calculations.

Responsibilities:

* tick conversion
* measure boundaries
* beat mapping
* tempo-based wall-clock conversion

## 9.6 `edit`

Editing command system.

Responsibilities:

* create/move/resize/delete notes
* track editing
* tempo/time signature edits
* batch edit transactions
* undo/redo foundation

## 9.7 `playback`

Playback engine.

Responsibilities:

* transport state
* event scheduling
* MIDI output emission
* current position tracking
* looping
* UI progress notifications

## 9.8 `recording`

Recording engine.

Responsibilities:

* MIDI input handling
* note-on/note-off pairing
* timestamp conversion
* overdub/replace policies

## 9.9 `metronome`

Metronome generation.

Responsibilities:

* click scheduling
* count-in
* downbeat accent
* recording assistance

## 9.10 `io`

Standard MIDI file I/O.

Responsibilities:

* parse MIDI file formats
* convert file contents into domain model
* export domain model to MIDI file

## 9.11 `persistence`

Native project format.

Responsibilities:

* save/load editable project data
* schema versioning
* preserve stable IDs and extra metadata

## 9.12 `device`

MIDI device abstraction.

Responsibilities:

* enumerate input/output devices
* open/close devices
* send MIDI output
* receive MIDI input
* isolate platform backends

## 9.13 `notify`

Structured notifications.

Responsibilities:

* publish core events
* deliver model changes and transport changes
* decouple producers from bridge/UI consumers

---

# 10. Saucer Integration Architecture

## 10.1 Role of Saucer

Saucer is the native host for the HTML/TypeScript UI. It should be treated as:

* the desktop windowing/UI container
* the WebView integration layer
* a bridge host between JS and native code

## 10.2 Recommended integration pattern

Each document window should have:

* a native `WindowController`
* a Saucer `WebView`
* a binding to a document ID
* a bridge dispatcher connected to the core façade

### Per window

```text
WindowController
  ├── Saucer window/webview
  ├── BridgeDispatcher
  ├── document_id
  └── UI event subscription
```

## 10.3 Bridge contract recommendation

Use **structured JSON messages** for MVP.

Why:

* simple to debug
* easy to inspect in both C++ and TypeScript
* good enough for an MVP
* fits naturally with web tooling

Later, if needed, it can evolve to a more compact schema.

---

# 11. UI ↔ Core Communication Model

## 11.1 General rule

The UI must not directly manipulate native state. It sends commands to the core through the bridge.

The core sends back:

* operation results
* snapshots
* incremental change events
* playback/recording notifications

## 11.2 Command flow

Example:

1. user clicks on score
2. UI interprets click according to active tool/grid/mode
3. UI sends command such as `CreateNote`
4. bridge decodes message
5. core validates and applies edit
6. core updates revision
7. core publishes `NoteCreated` or `DocumentPatched`
8. UI updates local state/rendering

## 11.3 Notification flow

Playback/recording and model changes should be pushed from core to UI as asynchronous notifications.

Typical notifications:

* transport position changed
* playback started/stopped
* recording started/stopped
* track updated
* note created/updated/deleted
* tempo map changed
* device list changed

## 11.4 Snapshot + patch model

Recommended for MVP:

* send a **document snapshot** when a window opens or a document loads
* send **incremental patch events** after edits
* send **throttled transport updates** during playback

This is much cleaner than constantly rebuilding the entire state.

---

# 12. Multi-Document / Multi-Window Architecture

You said the main window could contain multiple opened MIDIs, while also describing each client window. The architecture should support both possibilities without locking you in too early.

## 12.1 Recommended MVP interpretation

Implement the document system so it supports:

* multiple open documents
* one view/controller per document window initially

Later, this can evolve to:

* tabbed main window
* multi-document docking
* hybrid window/tab mode

## 12.2 Document manager

Introduce a `DocumentManager` in the core/app layer.

Responsibilities:

* create/open/close documents
* assign document IDs
* track dirty state
* route commands to the correct document
* coordinate save prompts

---

# 13. Core Threading Architecture

## 13.1 Recommended threads

At minimum:

* **UI/native thread**
* **core orchestration thread** or controlled access path
* **playback scheduler thread**
* **MIDI input callback/thread** depending on backend

## 13.2 Rules

* document mutation should happen in a controlled context
* playback thread should not perform heavy allocations or long locks
* recording input callbacks should enqueue events quickly
* UI thread should not block on long operations

## 13.3 Practical threading model

Recommended MVP model:

* most document edits happen on the main/native orchestration thread
* playback runs on its own scheduler thread
* recording input uses a lock-minimized queue
* notification delivery to UI is marshaled onto the appropriate bridge/UI-safe context

---

# 14. Native Core Build Architecture

## 14.1 Build system

Use **CMake** as the native build system.

Recommended minimum CMake version:

* modern enough for C++20 and good target-based configuration

## 14.2 Core CMake goals

The build should provide:

* clean target separation
* warnings enabled
* debug and release configs
* optional tests
* optional sanitizers for debug builds
* packaging-friendly output structure

## 14.3 Recommended target layout

Example conceptual targets:

* `midi_composer_core`
* `midi_composer_shell`
* `midi_composer_app`
* `midi_composer_tests`

For smaller MVP, this can be reduced to:

* one executable
* one core static library

### Recommended structure

* core logic as a library target
* application executable target links that library

This makes testing much easier.

## 14.4 CMake configuration expectations

The architecture document should require:

* `CMAKE_CXX_STANDARD 20`
* no compiler extensions unless necessary
* strict warnings on major compilers
* separate output dirs for binaries and libraries
* support for dependency fetching or vendoring
* configurable build type
* optional feature flags

## 14.5 Recommended options

Examples of CMake options worth adding:

* `MIDI_COMPOSER_BUILD_TESTS`
* `MIDI_COMPOSER_ENABLE_SANITIZERS`
* `MIDI_COMPOSER_ENABLE_LTO`
* `MIDI_COMPOSER_USE_BUNDLED_DEPENDENCIES`
* `MIDI_COMPOSER_BUILD_UI_ASSETS`
* `MIDI_COMPOSER_ENABLE_LOGGING`

---

# 15. Library Selection Guidelines

## 15.1 Header-only preference policy

The project should prefer header-only libraries for:

* logging
* JSON
* enums/string conversions
* lightweight utility helpers
* tests where practical

This reduces build complexity, especially early in the project.

## 15.2 Logging

Use a header-only logging library with:

* levels
* formatting
* optional compile-time disabling in release
* file/console sink flexibility if needed later

## 15.3 JSON

A header-only JSON library is strongly recommended because the bridge and native persistence will likely both need JSON.

Use cases:

* UI bridge messages
* native project persistence
* debugging snapshots

## 15.4 Testing

Choose a lightweight testing framework that fits cleanly in CMake and is easy to run in CI.

---

# 16. Native Project Persistence Architecture

## 16.1 Why native format is required

MIDI is not enough as the native save format because it does not preserve all editor semantics and future metadata cleanly.

## 16.2 Recommended MVP persistence format

Use a **JSON-based native project file** for MVP.

Possible extension:

* `.midicomp`
* `.mcproj`

## 16.3 What must be persisted

* composition metadata
* tracks
* notes
* controllers
* pitch bends
* tempo map
* time signature map
* version/schema info
* document metadata
* future-compatible extra metadata

---

# 17. UI Build Architecture

## 17.1 Build tool

Parcel is fine for MVP because it keeps setup light and developer-friendly.

## 17.2 Recommended UI stack

* TypeScript
* Lit for components
* Bootstrap for layout and baseline styling

This is a good productivity-oriented choice.

## 17.3 UI build outputs

The UI build should produce static assets that can be:

* served in development
* loaded from local packaged files in production

Recommended output:

* `ui/dist/`

The native app can then:

* load the dev server URL in development
* load packaged static files in production

## 17.4 TypeScript configuration expectations

The UI project should enforce:

* strict typing
* module-based structure
* path alias support if helpful
* source maps in dev
* no unchecked implicit any

---

# 18. Rendering Strategy for the Music Score

You raised an important open decision: **canvas or HTML**.

## 18.1 Recommendation for MVP

Use **HTML/CSS/SVG hybrid**, not pure canvas, unless you already know the score editor will become graphically heavy very quickly.

### Why

For MVP, HTML/SVG usually gives:

* easier interaction
* easier debugging
* easier hit testing
* easier styling
* easier inspection in browser dev tools
* easier accessibility hooks
* lower implementation complexity for many editor widgets

Canvas is powerful, but it shifts more burden to your own rendering engine.

## 18.2 Practical recommendation

Split the views by purpose:

### Score / notation-like rendering

Prefer **SVG** or a mixed HTML + SVG approach.

### Piano-roll or dense event grid later

Canvas may become useful there.

### Mixer and event tables

Use standard HTML components.

## 18.3 Recommendation for note symbols

For note glyphs and score symbols, use one of these approaches:

### Best MVP choice

Use **SVG assets or SVG-generated components**.

Why:

* scalable
* crisp rendering
* easy styling
* works well inside HTML-based UI
* easier than maintaining PNG variants

### Avoid as primary choice

PNG for notes is less ideal because:

* scaling quality issues
* multiple resolution assets needed
* harder theming

### Also possible

Draw symbols programmatically in SVG or canvas once the rendering rules mature.

## 18.4 Final recommendation on this topic

For MVP:

* **UI layout:** HTML/CSS
* **score graphics:** SVG
* **complex future dense editors:** consider canvas later

That gives you the fastest path with good maintainability.

---

# 19. UI View Architecture

The UI should support, per document window:

* collapsible score area
* collapsible mixer area
* collapsible MIDI events area

## 19.1 Suggested layout model

A three-pane split layout with collapsible panels.

Possible orientation:

* center: score/editor
* bottom: MIDI events
* side: mixer

or:

* center: score
* right: mixer
* bottom: MIDI events

This should be a UI-level layout concern, not a core concern.

## 19.2 Menu architecture

The native shell should own the top-level menu because this is a desktop app.

Menu actions are translated into app/core commands such as:

* new/open/save
* undo/redo
* preferences
* window management
* help/about

---

# 20. Asset Strategy

## 20.1 Static assets

The project will likely need:

* icons
* note glyphs
* transport icons
* UI theme assets

## 20.2 Recommended policy

* keep source assets in `ui/src/assets`
* generated/built assets go to `ui/dist`
* packaged app may copy built UI into `core/resources/web` or equivalent packaging location

## 20.3 Score symbol assets

Use:

* SVG files for noteheads, rests, clefs, and accidentals if asset-based
* or programmatic SVG generation if you want better flexibility later

---

# 21. Git Ignore Requirements

You asked for ignore files for both the C++/WebView project and the UI.

## 21.1 Root `.gitignore`

At repository root, include ignores for:

### C++ / CMake

* build directories
* generated CMake files
* compiled objects
* binaries
* test output
* debug artifacts

### IDE/editor

* `.idea/`
* `.vscode/`
* platform-specific user files

### OS junk

* `.DS_Store`
* `Thumbs.db`

### UI / Node

* `node_modules/`
* `dist/`
* cache folders
* package manager logs

### Example categories to include

```text
/build/
/out/
/bin/
/lib/
/cmake-build-*/
/CMakeFiles/
/CMakeCache.txt
/compile_commands.json

/node_modules/
/ui/dist/
/.parcel-cache/

.DS_Store
Thumbs.db
.vscode/
.idea/
```

## 21.2 `ui/.gitignore`

The UI folder should also have a local `.gitignore` covering:

* `node_modules`
* `dist`
* Parcel cache
* logs
* temp files

This is useful even if the root ignore already catches them.

---

# 22. Configuration Files Expected in UI

The UI folder should contain at minimum:

* `package.json`
* `tsconfig.json`
* `.gitignore`

Potentially also:

* `.npmrc`
* bundler config if needed
* lint/format config
* test config later

## 22.1 `package.json`

Should define:

* build
* dev
* clean
* typecheck

## 22.2 `tsconfig.json`

Should enable:

* `strict`
* target/module appropriate for bundler
* DOM support
* source maps in dev setup
* clean path configuration

---

# 23. Document and Window State Architecture

There are two distinct state classes in the app:

## 23.1 Core document state

Lives in native core:

* composition
* playback state
* recording state
* dirty flag
* revision

## 23.2 UI view state

Lives in frontend:

* active tool
* zoom
* panel collapse state
* current selection
* scroll position
* edit/play mode rendering state

This separation should be explicit.

---

# 24. Error Handling and Diagnostics

## 24.1 Native core

Use:

* structured error results
* assertions for programming mistakes
* logs for diagnostics

## 24.2 UI

Use:

* typed command result handling
* visible non-fatal error dialogs/messages
* console logging in dev mode

## 24.3 Bridge

All bridge errors should be:

* structured
* versioned
* debuggable from both sides

---

# 25. Testing Architecture

## 25.1 Core tests

Test at least:

* note editing
* tempo/time signature conversions
* MIDI import/export basics
* playback event generation
* recording note pairing

## 25.2 UI tests

For MVP, keep this light:

* TypeScript type checks
* some component/unit tests later
* manual dev testing through Saucer integration early on

## 25.3 Integration tests

Later, useful targets include:

* bridge protocol tests
* document open/save/load
* MIDI roundtrip tests

---

# 26. Packaging / Dev Mode Architecture

## 26.1 Development mode

Recommended:

* UI runs from Parcel dev server
* Saucer loads local dev URL
* core runs native executable normally

This gives fast UI iteration.

## 26.2 Production mode

Recommended:

* build UI static bundle
* package assets with native app
* Saucer loads local packaged HTML entrypoint

The architecture should support both from the start.

---

# 27. Open Decisions and Recommended Defaults

## 27.1 Score rendering technology

Open question: HTML vs canvas

Recommended default:

* HTML + SVG for MVP

## 27.2 Symbol rendering

Open question: PNG vs SVG vs code-drawn

Recommended default:

* SVG first

## 27.3 UI framework

Open question: Bootstrap or similar

Recommended default:

* Bootstrap + custom CSS + Lit

## 27.4 Bundler

Open question: Parcel

Recommended default:

* Parcel for MVP

## 27.5 Multi-document UX

Open question: tabs vs windows

Recommended default:

* architect for multiple documents
* implement per-document windows first if simpler

---

# 28. Additional Items That Should Be Included

A few things are worth adding now because they usually get forgotten.

## 28.1 Versioned bridge protocol

The UI/native message format should include a protocol version.

## 28.2 Feature flags

Useful for enabling unfinished editor features safely.

## 28.3 Revision-based state sync

Every document change should increment a revision number.

## 28.4 Dirty-state tracking

Needed for save prompts and multiple document handling.

## 28.5 Resource lifecycle policy

All native resources must be RAII-managed:

* device handles
* threads
* window bindings
* subscriptions
* file handles

## 28.6 Logging categories

Define log categories early:

* app
* bridge
* playback
* recording
* io
* persistence
* device
* ui-host

## 28.7 Dev build ergonomics

Add support for:

* sanitizers
* warnings as errors optionally
* compile commands generation
* debug symbols

---

# 29. Recommended Baseline Decisions

To keep momentum and avoid overthinking, I would lock these in now:

* **Native core:** C++20
* **Desktop host:** Saucer
* **Build:** CMake
* **Source location:** `core/src`
* **UI:** TypeScript + Lit + Bootstrap
* **UI build:** Parcel
* **Score rendering MVP:** SVG within HTML-based UI
* **Note symbols:** SVG assets/components, not PNG
* **Bridge:** JSON messages
* **Persistence:** JSON native project format for MVP
* **Logging:** header-only logger
* **Dependency style:** prefer header-only where practical
* **Repo shape:** `core/` and `ui/` sibling folders

---

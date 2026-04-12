# MIDI Composer — CMake and Project Bootstrap Spec

## 1. Purpose

This document defines how the project should be bootstrapped and built from day one.

It covers:

* repository layout
* CMake organization
* native target structure
* dependency strategy
* build options
* development vs production UI integration
* starter configuration files
* `.gitignore` setup
* bootstrap sequence for the first working version

The goals are:

* fast initial development
* clean separation between native core and UI
* portable build structure
* maintainable dependency management
* smooth developer workflow

---

# 2. Repository Layout

Recommended repository structure:

```text
midi-composer/
  .gitignore
  README.md

  core/
    CMakeLists.txt
    cmake/
      warnings.cmake
      sanitizers.cmake
      options.cmake
      dependencies.cmake
      packaging.cmake

    external/
    resources/
      icons/
      web/

    src/
      app/
      base/
      shell/
      ui_bridge/
      music/
      timeline/
      project/
      edit/
      playback/
      recording/
      metronome/
      midi/
      io/
      persistence/
      device/
      notify/

    tests/

  ui/
    package.json
    tsconfig.json
    .gitignore
    src/
    public/
    dist/
```

## 2.1 Required folder rule

The native C++ source code must live inside:

```text
core/src/
```

The UI source code must live inside:

```text
ui/src/
```

That should be treated as a hard project rule.

---

# 3. Build System Overview

## 3.1 Native build system

The native application shall use:

* **CMake**
* **C++20 minimum**
* target-based configuration
* out-of-source builds only

## 3.2 UI build system

The UI shall use:

* Node-based toolchain
* TypeScript
* Parcel for MVP
* package manager scripts for dev/build/typecheck

## 3.3 Integration strategy

The native app and the UI are built separately but integrated through:

* development mode: Saucer loads a dev server URL
* production mode: Saucer loads built static files from packaged assets

---

# 4. CMake Version and Language Standards

## 4.1 Minimum CMake version

Use a modern CMake version suitable for:

* target-based configuration
* C++20
* FetchContent or modern dependency management
* generator expressions and modern options

Recommended baseline:

* **CMake 3.25+**

That is a comfortable modern baseline.

## 4.2 C++ standard

The project shall require:

* **C++20**
* no compiler-specific language extensions by default

Recommended settings:

* `CMAKE_CXX_STANDARD 20`
* `CMAKE_CXX_STANDARD_REQUIRED ON`
* `CMAKE_CXX_EXTENSIONS OFF`

---

# 5. Native Target Structure

The build should be organized around a few clean targets.

## 5.1 Recommended initial targets

### `midi_composer_core`

A library target containing most native engine and application logic.

### `midi_composer_app`

The executable target containing:

* `main`
* shell startup
* Saucer window integration

### `midi_composer_tests`

A test executable or group of test targets.

## 5.2 Recommended target model

For MVP:

* `midi_composer_core` as a **static library**
* `midi_composer_app` as the executable
* tests link against `midi_composer_core`

This is the best default because it:

* keeps app startup thin
* improves testability
* avoids stuffing everything into `main.cpp`

---

# 6. Source Grouping in CMake

Recommended source ownership:

## 6.1 `midi_composer_core`

Includes:

* `app/`
* `base/`
* `music/`
* `timeline/`
* `project/`
* `edit/`
* `playback/`
* `recording/`
* `metronome/`
* `midi/`
* `io/`
* `persistence/`
* `device/`
* `notify/`
* `ui_bridge/`

## 6.2 `midi_composer_app`

Includes:

* `shell/main.cpp`
* `shell/application.cpp`
* `shell/window_controller.cpp`
* menu and shell startup files

If later you want stricter separation, `ui_bridge` could move closer to app, but for MVP it is acceptable in core-facing code.

---

# 7. Suggested Top-Level CMake Layout

Inside `core/CMakeLists.txt`, the structure should roughly follow this order:

1. minimum CMake version
2. project declaration
3. language standard setup
4. include custom cmake modules
5. options
6. dependency loading
7. target creation
8. warnings/sanitizers settings
9. tests
10. packaging/install rules if enabled

---

# 8. Recommended CMake Options

These options should exist early because they improve dev workflow a lot.

## 8.1 Build options

Recommended options:

* `MIDI_COMPOSER_BUILD_TESTS`
* `MIDI_COMPOSER_ENABLE_SANITIZERS`
* `MIDI_COMPOSER_ENABLE_LTO`
* `MIDI_COMPOSER_WARNINGS_AS_ERRORS`
* `MIDI_COMPOSER_ENABLE_LOGGING`
* `MIDI_COMPOSER_BUILD_UI_ASSETS`
* `MIDI_COMPOSER_USE_BUNDLED_DEPENDENCIES`
* `MIDI_COMPOSER_DEV_WEB_UI`

## 8.2 Recommended defaults

For local development:

* tests: ON
* logging: ON
* warnings as errors: OFF by default, maybe ON in CI
* sanitizers: OFF by default, ON when explicitly requested
* dev web UI: ON in debug-oriented workflows

---

# 9. Dependency Strategy

You asked to prioritize header-only libraries. That is a good direction.

## 9.1 Policy

Prefer header-only libraries for:

* logging
* JSON
* utility helpers
* test helpers if practical

But do not force header-only choices when:

* platform integration needs compiled code
* performance or maintainability strongly favors another approach

## 9.2 Dependency sourcing strategy

Recommended order of preference:

### A. system package or package manager integration for larger/platform-sensitive libs

Good for libraries like Saucer if appropriate for the platform workflow.

### B. `FetchContent` for smaller libraries or reproducible setup

Useful for header-only libs and testing libs.

### C. vendored under `core/external/` only when justified

Use this sparingly.

## 9.3 Recommendation

Use CMake `FetchContent` for most lightweight dependencies in early development.

That gives:

* easy onboarding
* reproducible builds
* less manual environment setup

---

# 10. Recommended Dependency Categories

## 10.1 Required native dependency categories

The architecture expects these dependency types:

* WebView/window hosting library: **Saucer**
* header-only logging library
* header-only JSON library
* testing library
* optional formatting/string utility helpers

## 10.2 Logging library requirements

The logging lib should support:

* log levels
* formatted messages
* simple macros or function API
* zero/minimal setup
* compile-time stripping or disabling if desired

## 10.3 JSON library requirements

The JSON library should support:

* serialization/deserialization
* easy DOM-style use for bridge messages
* conversion helpers to DTOs
* strong ecosystem/documentation

It will be used for:

* UI bridge protocol
* native project file format
* debugging snapshots

---

# 11. CMake Module Files

Recommended `core/cmake/` helper files:

```text
core/cmake/
  options.cmake
  warnings.cmake
  sanitizers.cmake
  dependencies.cmake
  packaging.cmake
```

## 11.1 `options.cmake`

Responsibilities:

* define project options
* provide sane defaults

## 11.2 `warnings.cmake`

Responsibilities:

* set compiler warnings per compiler
* optionally enable warnings-as-errors

## 11.3 `sanitizers.cmake`

Responsibilities:

* enable AddressSanitizer / UndefinedBehaviorSanitizer for supported compilers/configs

## 11.4 `dependencies.cmake`

Responsibilities:

* declare and fetch dependencies
* centralize dependency management

## 11.5 `packaging.cmake`

Responsibilities:

* install rules
* resource copy rules
* future packaging logic

---

# 12. Compiler Warning Policy

This is worth locking early.

## 12.1 Goal

The code should compile cleanly with strong warnings enabled.

## 12.2 Recommended warning behavior

For GCC/Clang-like compilers, enable a strong but practical set of warnings.

For MSVC, use high warning level but avoid making life miserable with noisy unrelated diagnostics.

## 12.3 Warnings-as-errors

Recommended:

* optional locally
* enabled in CI later

This avoids slowing down early experimentation while still encouraging discipline.

---

# 13. Sanitizer Policy

## 13.1 Why

This project includes:

* threading
* device I/O
* timing code
* C++ ownership-sensitive code

Sanitizers are extremely useful.

## 13.2 Recommended support

When enabled and supported:

* AddressSanitizer
* UndefinedBehaviorSanitizer

ThreadSanitizer can be helpful later, but it often complicates setup and third-party libs.

## 13.3 Recommended rule

Enable sanitizers only in non-release development builds.

---

# 14. LTO Policy

## 14.1 Purpose

Link-time optimization can improve production builds.

## 14.2 Recommendation

Support it as an option:

* OFF by default in dev
* ON in optimized release or packaging builds if stable

---

# 15. Compile Commands

## 15.1 Requirement

The build should generate `compile_commands.json`.

This helps:

* clangd
* editor tooling
* code navigation
* refactoring support

## 15.2 Recommendation

Set:

* `CMAKE_EXPORT_COMPILE_COMMANDS ON`

at least for supported generators.

---

# 16. Output Directory Strategy

The project should avoid scattering outputs.

## 16.1 Recommended output structure

For native builds, organize outputs under the build directory in predictable folders such as:

* `bin/`
* `lib/`

## 16.2 Rule

No generated build artifacts should go inside source folders.

Use out-of-source builds only.

---

# 17. UI Integration Strategy in CMake

This is one of the most important practical parts.

## 17.1 Two modes

### Development mode

Saucer loads the UI from a local dev server, such as Parcel dev server.

### Production mode

Saucer loads local built files from packaged UI assets.

## 17.2 Recommended CMake option

Use:

* `MIDI_COMPOSER_DEV_WEB_UI`

When ON:

* native app is built expecting a dev server URL
* useful in active frontend development

When OFF:

* native app loads packaged assets

## 17.3 Optional build step integration

Use:

* `MIDI_COMPOSER_BUILD_UI_ASSETS`

When ON:

* CMake may invoke UI build commands during native packaging-oriented builds

For MVP, this can remain optional rather than forcing Node during every native build.

That is cleaner and friendlier.

---

# 18. Native Resource Layout

Recommended native resource structure:

```text
core/resources/
  icons/
  web/
```

## 18.1 Role of `core/resources/web`

This folder may contain the built UI assets copied from `ui/dist` for packaged or local production-like runs.

## 18.2 Rule

Generated built UI files should not be manually edited in `core/resources/web`.

They are build artifacts or packaging inputs.

---

# 19. Bootstrap Behavior of the App

The very first working app should be able to:

* start the native executable
* create a Saucer window
* load the UI
* establish the bridge
* respond to a simple test command from UI
* show a simple rendered shell screen

That is the minimum bootstrap milestone.

## 19.1 Phase-1 bootstrap milestone

At first successful bootstrap:

* window opens
* UI loads
* “Core connected” or similar test message works
* a command like `NewProjectCommand` succeeds
* a simple document snapshot is displayed

That is the first end-to-end vertical slice.

---

# 20. UI Project Bootstrap Requirements

Inside `ui/`, the initial files should include:

* `package.json`
* `tsconfig.json`
* `.gitignore`

Optionally later:

* lint config
* format config
* test config

## 20.1 `package.json`

Should include scripts like:

* `dev`
* `build`
* `clean`
* `typecheck`

## 20.2 `tsconfig.json`

Should enable:

* `strict`
* sensible module target for bundler
* source maps in development
* DOM libs
* no implicit any

---

# 21. Root `.gitignore` Spec

You asked for git ignore setup for the C++ and WebView project.

Recommended root `.gitignore` content should cover these categories.

## 21.1 CMake and native build artifacts

Ignore:

* build directories
* CMake generated files
* binaries
* libraries
* object files
* debug symbols where appropriate
* test binaries

Examples:

```gitignore
/build/
/out/
/bin/
/lib/
/cmake-build-*/
/CMakeFiles/
/CMakeCache.txt
/compile_commands.json
/Makefile
```

You may or may not want to ignore `compile_commands.json`; many teams do. If you use clangd heavily, you might keep it build-local only anyway.

## 21.2 C++ artifacts

Ignore common compiled outputs:

```gitignore
*.o
*.obj
*.a
*.lib
*.so
*.dylib
*.dll
*.exe
*.pdb
```

## 21.3 UI / Node artifacts

Ignore:

```gitignore
/node_modules/
/ui/dist/
/ui/.parcel-cache/
npm-debug.log*
yarn-debug.log*
yarn-error.log*
pnpm-debug.log*
```

## 21.4 IDE/editor files

Ignore:

```gitignore
.vscode/
.idea/
*.user
*.suo
```

## 21.5 OS junk

Ignore:

```gitignore
.DS_Store
Thumbs.db
```

---

# 22. `ui/.gitignore` Spec

The `ui/` folder should also have a local `.gitignore`.

Recommended entries:

```gitignore
node_modules/
dist/
.parcel-cache/
*.log
```

This is useful even if the root ignore already covers them.

---

# 23. `README.md` Bootstrap Expectations

The root `README.md` should explain:

* project purpose
* repo layout
* prerequisites
* how to build core
* how to run UI dev server
* how to run the app in dev mode
* how to build the UI for production
* how to run tests

This matters more than people think in week one.

---

# 24. Recommended Native Bootstrap Targets

A practical first bootstrap should aim for these milestones.

## 24.1 Milestone 1 — Native shell boots

* CMake config works
* executable builds
* basic native window opens

## 24.2 Milestone 2 — Saucer UI loads

* Saucer integrated
* simple HTML page displayed
* dev/prod UI path switch works

## 24.3 Milestone 3 — Bridge connected

* UI sends ping/test command
* C++ responds
* response appears in UI

## 24.4 Milestone 4 — First document flow

* `NewProjectCommand`
* `DocumentSnapshotEvent`
* basic score shell visible

That is the correct first end-to-end implementation path.

---

# 25. Suggested Initial `core/CMakeLists.txt` Responsibilities

The main native CMake file should:

* define project
* load cmake helper modules
* define build options
* fetch/include dependencies
* create `midi_composer_core`
* create `midi_composer_app`
* configure include directories
* set compile features
* apply warnings and optional sanitizers
* add tests if enabled

It should remain fairly short by delegating complexity to helper files.

---

# 26. Include Directory Strategy

There are two acceptable models.

## 26.1 Simple MVP model

Headers live alongside `.cpp` files inside `core/src`.

This is perfectly acceptable early on.

Example:

```text
core/src/music/composition.hpp
core/src/music/composition.cpp
```

## 26.2 Split include model

Later, if you want a cleaner public/private separation:

* public headers in `core/include/`
* implementation headers and sources in `core/src/`

For MVP, I recommend the simpler colocated-header model unless you already know you need library-style public API packaging.

---

# 27. Test Bootstrap Strategy

## 27.1 Build option

Tests should be gated behind:

* `MIDI_COMPOSER_BUILD_TESTS`

## 27.2 Test scope for first pass

Start with tests for:

* strong IDs / base utilities
* note duration/tick logic
* measure calculation
* notation fragmentation helpers later
* basic edit operations

Do not wait until playback to start testing.

---

# 28. Platform and Toolchain Considerations

## 28.1 Cross-platform intent

The architecture should remain friendly to:

* Windows
* Linux
* macOS

## 28.2 Platform isolation

Anything platform-specific should be kept behind modules like:

* `device/backend/`
* possibly `shell/platform/` later

## 28.3 Toolchain expectations

CMake should support common compilers:

* MSVC
* Clang
* GCC

with compiler-specific warnings configured in helper modules.

---

# 29. Recommended First Bootstrap File Set

Here is a realistic first file set to create.

## 29.1 Root

```text
midi-composer/
  .gitignore
  README.md
```

## 29.2 Core

```text
core/
  CMakeLists.txt
  cmake/
    options.cmake
    warnings.cmake
    sanitizers.cmake
    dependencies.cmake
  src/
    shell/
      main.cpp
      application.hpp
      application.cpp
      window_controller.hpp
      window_controller.cpp
    app/
      core_facade.hpp
      core_facade.cpp
    ui_bridge/
      bridge_dispatcher.hpp
      bridge_dispatcher.cpp
    base/
      error.hpp
      strong_id.hpp
      logger.hpp
  tests/
```

## 29.3 UI

```text
ui/
  package.json
  tsconfig.json
  .gitignore
  src/
    app/
      main.ts
    bridge/
      coreBridge.ts
    components/
      app-root.ts
    styles/
      main.css
  public/
```

That is enough for the first vertical slice.

---

# 30. Recommended Bootstrap Sequence

Here is the order I’d strongly recommend.

## 30.1 Step 1

Create repository structure and ignore files.

## 30.2 Step 2

Create minimal `core/CMakeLists.txt` and build a tiny executable.

## 30.3 Step 3

Add Saucer and open a native window.

## 30.4 Step 4

Create minimal UI project with Parcel + TypeScript.

## 30.5 Step 5

Load UI inside Saucer in dev mode.

## 30.6 Step 6

Implement a minimal JSON bridge:

* ping
* test command
* test response

## 30.7 Step 7

Add `CoreFacade` and a dummy `NewProjectCommand`.

## 30.8 Step 8

Render first document shell in UI.

## 30.9 Step 9

Add snapshot event flow.

## 30.10 Step 10

Begin real score view and domain model integration.

That sequence keeps risk low and gives visible progress early.

---

# 31. Recommended Initial CMake Decisions to Lock Now

Here’s the concrete baseline I’d lock in:

* **CMake minimum:** 3.25+
* **C++ standard:** 20
* **Extensions:** off
* **Core target:** static library
* **App target:** executable linked to core library
* **Dependency style:** prefer header-only + FetchContent where practical
* **Warnings:** strong, target-based
* **Sanitizers:** optional via CMake option
* **Tests:** optional but enabled by default for dev
* **UI integration:** dev URL mode and packaged assets mode
* **Source structure:** `core/src`, `ui/src`
* **Generated assets:** outside source folders except intentional packaged resource copy locations

---

# 32. Extra Items Worth Adding Now

A few things are easy to forget and useful to formalize.

## 32.1 Build presets

Consider adding CMake presets later for:

* debug
* release
* sanitizer build

This is not mandatory for day one, but very useful.

## 32.2 Formatting tools

You should strongly consider adding:

* `clang-format` for C++
* Prettier for UI

## 32.3 CI readiness

Even before CI is added, structure the build so it can later run:

* configure
* build
* test
* UI typecheck
* UI build

without weird local assumptions.

## 32.4 Version header

It is useful to generate an app version header or config early, though not mandatory for bootstrap.

---

# 33. Final Bootstrap Goal

A successful initial bootstrap is achieved when:

* the repository structure exists
* native build works with CMake
* UI build works with Parcel/TypeScript
* Saucer loads the UI
* bridge sends and receives JSON messages
* a native-backed `NewProjectCommand` works
* the UI renders a basic multi-document-capable shell

That is the right “foundation complete” milestone.

---

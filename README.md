# MIDI Composer

A desktop MIDI composition editor with a native C++ core and a WebView UI.

## Project Structure

- `core/`: Native C++ core engine and application shell.
- `ui/`: TypeScript/Lit UI.
- `docs/`: Design and architecture documentation.

## Getting Started

### Prerequisites

- C++20 compiler (MSVC, GCC, or Clang)
- CMake 3.25+
- Node.js and npm/pnpm

## Development build

Two processes: the UI runs on Parcel's dev server and the app points its webview
at it, so a UI change is a browser reload rather than a rebuild.

```powershell
cd ui; npm install; npm run dev
```

```powershell
cd core; cmake -S . -B build; cmake --build build --config Release
```

`MIDI_COMPOSER_DEV_WEB_UI` is `ON` by default, which is what selects the dev
server. Dev tools are enabled in this mode.

## Release build

```powershell
cd core
cmake -S . -B build-release -DMIDI_COMPOSER_DEV_WEB_UI=OFF
cmake --build build-release --config Release
```

With the option off there is no dev server to point at. The build runs the UI's
production bundler itself and packs the result into `ui.pak` — a zip — next to
the executable; at startup the app reads it into memory and serves it to the
webview over saucer's embedded-file scheme. Nothing is served over HTTP, no port
is opened, and there are no loose UI files for anything to tamper with.

Shipping therefore means two files: `midi_composer_app.exe` and `ui.pak`. The app
refuses to start without the bundle and says so in a dialog rather than opening a
blank window.

Node is required for a release build, since the UI has to be built to be packed.
To repack after a UI change without rebuilding the C++:

```powershell
cmake --build build-release --config Release --target midi_composer_ui_pak
```

The C runtime is linked statically, so the executable depends only on Windows
system libraries — there is no Visual C++ redistributable to install alongside it.

## Installer

```powershell
cmake --build build-release --config Release --target midi_composer_installer
```

Produces `build-release/installer/MIDIComposer-<version>-setup.exe` from
[`installer/midi_composer.nsi`](installer/midi_composer.nsi). Requires NSIS
(`winget install NSIS.NSIS`); without it the target reports that and fails, and
the rest of the build is unaffected.

The installer offers per-machine or per-user installation, adds a Start Menu
entry and an uninstaller, and refuses to run on 32-bit Windows. It also checks
for the Microsoft Edge WebView2 runtime and installs it if missing — the UI is a
webview, so without it the application opens an empty window.

Silent install and uninstall:

```powershell
MIDIComposer-0.1.0-setup.exe /S /CurrentUser /D=C:\Tools\MIDIComposer
```

```powershell
"C:\Tools\MIDIComposer\uninstall.exe" /S
```

`/AllUsers` installs per-machine and needs elevation. `/D=` must come last and
takes an unquoted path.

## Release pipeline

[`.github/workflows/windows.yml`](.github/workflows/windows.yml) runs on `v*`
tags, and manually via *Run workflow*. It typechecks and tests the UI, builds the
core in the release configuration with tests enabled, runs the core suite through
`ctest`, and builds the installer, uploading the application (executable plus
`ui.pak`) and the installer as artifacts.

A tag also publishes a GitHub release with the installer and a zip of the
portable build — the binaries the build job already tested, not a second build of
the same source. A manual run stops at the artifacts.

Nothing runs on an ordinary push, so the tests are yours to run locally before
tagging.

The workflow is named for the platform rather than `ci`, because Linux support
will need its own: the core still has Win32-only pieces (native file dialogs, the
executable-relative bundle path) and the webview backend differs.

## Tests

```powershell
cd ui; npm test
```

```powershell
cd core; cmake -S . -B build -DMIDI_COMPOSER_BUILD_TESTS=ON; cmake --build build --config Release; .\build\Release\midi_composer_tests.exe
```

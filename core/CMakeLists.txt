cmake_minimum_required(VERSION 3.25)

project(MIDIComposer
    VERSION 0.3.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The C runtime is linked statically so a shipped build is the executable and
# its UI bundle, nothing else. Otherwise the app needs MSVCP140 / VCRUNTIME140
# on the target machine and simply fails to launch where they are absent — the
# installer would have to carry and bootstrap the redistributable to fix a
# problem this line removes. Set before the dependencies so everything fetched
# below is built the same way; mixing the two runtimes in one binary is a
# reliable source of heap corruption.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Symbols in the release build too.
    #
    # A .pdb changes nothing about the code that ships -- /Zi is not /Od -- but
    # it is the difference between a crash report that names a function and one
    # that names a hex offset into a binary nobody kept. The file stays beside
    # the executable in the build tree and is simply not installed.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>
                     $<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>)
endif()

include(options)
include(warnings)
include(dependencies)

if(MIDI_COMPOSER_DEV_WEB_UI)
    add_compile_definitions(MIDI_COMPOSER_DEV_WEB_UI)
endif()

# Core Library
add_library(midi_composer_core STATIC)

target_sources(midi_composer_core
    PRIVATE
        src/base/logger.hpp
        src/base/error.hpp
        src/base/strong_id.hpp
        src/base/json_types.hpp
        src/timeline/tick.hpp
        src/app/core_facade.hpp
        src/app/core_facade.cpp
        src/app/document_manager.hpp
        src/app/document_manager.cpp
        src/app/preferences.cpp
        src/device/audio_device.hpp
        src/device/audio_device.cpp
        src/device/midi_service.hpp
        src/device/midi_service.cpp
        src/edit/edit_service.hpp
        src/edit/edit_service.cpp
        src/playback/audio_mixer.hpp
        src/playback/audio_mixer.cpp
        src/playback/clap_instance.hpp
        src/playback/clap_library.hpp
        src/playback/clap_library.cpp
        src/playback/clap_instance.cpp
        src/playback/internal_synth_output.hpp
        src/playback/internal_synth_output.cpp
        src/playback/output_plugin.hpp
        src/playback/routing_output.hpp
        src/playback/routing_output.cpp
        src/playback/gaussian_table.hpp
        src/playback/sample_bank.hpp
        src/playback/spc700_output.hpp
        src/playback/spc700_output.cpp
        src/playback/playback_engine.hpp
        src/playback/playback_engine.cpp
        src/playback/system_midi_output.hpp
        src/playback/system_midi_output.cpp
        src/music/composition.hpp
        src/music/composition.cpp
        src/music/midi_events.hpp
        src/music/track.hpp
        src/music/track.cpp
        src/music/tempo_map.hpp
        src/music/tempo_map.cpp
        src/music/time_signature_map.hpp
        src/music/time_signature_map.cpp
        src/project/project_document.hpp
        src/project/project_document.cpp
        src/project/undo_history.hpp
        src/project/project_serializer.hpp
        src/project/project_serializer.cpp
        src/io/midi_file.hpp
        src/io/midi_file.cpp
        src/io/wav_file.hpp
        src/io/wav_file.cpp
        src/io/sf2_file.hpp
        src/io/sf2_file.cpp
        src/io/brr.hpp
        src/io/brr.cpp
        src/io/sample_pitch.hpp
        src/io/sample_pitch.cpp
        src/io/spc_file.hpp
        src/io/spc_file.cpp
        src/shell/application.hpp
        src/shell/application.cpp
        src/shell/com_apartment.hpp
        src/shell/com_apartment.cpp
        src/shell/crash_report.hpp
        src/shell/crash_report.cpp
        src/shell/file_dialogs.hpp
        src/shell/file_dialogs.cpp
        src/shell/unsaved_changes.hpp
        src/shell/unsaved_changes.cpp
        src/shell/ui_bundle.hpp
        src/shell/ui_bundle.cpp
        src/ui_bridge/bridge_dispatcher.hpp
        src/ui_bridge/bridge_dispatcher.cpp
)

target_include_directories(midi_composer_core
    PUBLIC
        src
)

target_link_libraries(midi_composer_core
    PUBLIC
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        saucer::saucer
        fmt::fmt
        libremidi::libremidi
    PRIVATE
        miniz
        miniaudio
)

# PUBLIC because it is: ClapInstance takes a clap_plugin_t and hands back a
# clap_host_t, so anything including it needs the headers. Hiding them would
# mean an opaque handle and an out-of-line destructor for no gain here.
target_link_libraries(midi_composer_core PUBLIC clap_headers)

if(WIN32)
    # dbghelp turns a crash address into a function and a line. Without it the
    # log can only say the process died at 0x00000000000da28b, which is a fact
    # nobody can act on.
    target_link_libraries(midi_composer_core PUBLIC winmm.lib comdlg32.lib dbghelp.lib)
endif()

target_compile_definitions(midi_composer_core
    PUBLIC
        SPDLOG_FMT_EXTERNAL
        # Otherwise SPDLOG_DEBUG compiles to nothing. spdlog gates the macro at
        # compile time on this, and it defaults to info -- so every MC_LOG_DEBUG
        # in this project was being removed by the preprocessor, and the runtime
        # level the logger sets had nothing left to let through. What the level
        # is at runtime stays the logger's decision.
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    PRIVATE
        # The one place the version is written down is project() above. The
        # installer already read it from there; the About box used to carry its
        # own copy and drifted two releases behind.
        MIDI_COMPOSER_VERSION="${PROJECT_VERSION}"
)

set_project_warnings(midi_composer_core)

# Main Application
add_executable(midi_composer_app
    src/shell/main.cpp
)

# A windowed application, not a console one.
#
# Built as a console app, double-clicking it opens a black console window
# *and* the real one, often behind it -- and closing the console, which is the
# natural thing to do with a window full of log lines you did not ask for,
# kills the application. That is indistinguishable from "it will not start".
#
# /ENTRY:mainCRTStartup keeps plain `main()`: the alternative is a WinMain that
# exists only to satisfy the linker. The log still reaches a file (see
# base/logger.hpp), which is what a windowed build needs instead of stdout.
if(MSVC)
    set_target_properties(midi_composer_app PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

target_link_libraries(midi_composer_app
    PRIVATE
        midi_composer_core
        saucer::saucer
)

set_project_warnings(midi_composer_app)

# ── Packed UI ────────────────────────────────────────────────────────────────
#
# A development build points the webview at Parcel's dev server. A shipped one
# has no server to point at, so the built UI is packed into a zip next to the
# executable and served from memory (see src/shell/ui_bundle.cpp).
#
# The pack always runs rather than being wired to a dependency list: the UI's
# sources are outside this project and the bundle filename carries a content
# hash, so any file list here would go stale silently — and a stale pak is worse
# than a redundant rebuild of something Parcel does in under a second.
if(NOT MIDI_COMPOSER_DEV_WEB_UI)
    set(MIDI_COMPOSER_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")

    # npm.cmd first: a Node install on Windows ships both it and an extensionless
    # `npm` shell script, and find_program only ever appends .exe/.com, so the
    # shell script wins by name order and then cannot be executed.
    find_program(MIDI_COMPOSER_NPM NAMES npm.cmd npm)
    if(NOT MIDI_COMPOSER_NPM)
        message(FATAL_ERROR
            "npm was not found, and a release build has to build the UI to pack it. "
            "Install Node, or configure with -DMIDI_COMPOSER_DEV_WEB_UI=ON to use "
            "the dev server instead.")
    endif()

    # One command, not three: see the note at the top of pack_ui.cmake for why
    # chaining npm with anything else here does not survive on Windows.
    add_custom_target(midi_composer_ui_pak
        COMMAND ${CMAKE_COMMAND}
                -DUI_DIR=${MIDI_COMPOSER_UI_DIR}
                -DUI_PAK=$<TARGET_FILE_DIR:midi_composer_app>/ui.pak
                -DNPM=${MIDI_COMPOSER_NPM}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pack_ui.cmake"
        COMMENT "Building and packing the UI into ui.pak"
        VERBATIM
    )

    add_dependencies(midi_composer_app midi_composer_ui_pak)

    # ── Installer ────────────────────────────────────────────────────────────
    #
    # A hand-written .nsi rather than CPack's NSIS generator: the WebView2
    # runtime has to be detected and bootstrapped at install time, and through
    # CPack that means a wall of escaped NSIS embedded in CMake variables.
    # As its own script it stays readable and can be run without CMake at all.
    #
    # Optional on purpose. Not having NSIS should cost you the installer, not
    # the build.
    find_program(MIDI_COMPOSER_MAKENSIS
        NAMES makensis
        PATHS
            "$ENV{ProgramFiles\(x86\)}/NSIS"
            "$ENV{ProgramFiles}/NSIS"
    )

    set(MIDI_COMPOSER_INSTALLER_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer")

    if(MIDI_COMPOSER_MAKENSIS)
        add_custom_target(midi_composer_installer
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MIDI_COMPOSER_INSTALLER_DIR}"
            COMMAND "${MIDI_COMPOSER_MAKENSIS}"
                    /DSOURCE_DIR=$<SHELL_PATH:$<TARGET_FILE_DIR:midi_composer_app>>
                    /DAPP_VERSION=${PROJECT_VERSION}
                    /DOUTPUT_DIR=$<SHELL_PATH:${MIDI_COMPOSER_INSTALLER_DIR}>
                    "${CMAKE_CURRENT_SOURCE_DIR}/../installer/midi_composer.nsi"
            DEPENDS midi_composer_app
            COMMENT "Building the installer with NSIS"
            VERBATIM
        )
        # The installer picks the payload up from the build output, so the UI
        # bundle has to be current — not whatever a previous build left there.
        add_dependencies(midi_composer_installer midi_composer_ui_pak)
    else()
        add_custom_target(midi_composer_installer
            COMMAND ${CMAKE_COMMAND} -E false
            COMMENT "NSIS (makensis) was not found. Install it (winget install NSIS.NSIS) and re-run CMake."
            VERBATIM
        )
    endif()
endif()

# Tests
if(MIDI_COMPOSER_BUILD_TESTS)
    enable_testing()

    add_executable(midi_composer_tests
        tests/main.cpp
        tests/clap_tests.cpp
        tests/echo_tests.cpp
        tests/edit_service_tests.cpp
        tests/file_filter_tests.cpp
        tests/metronome_tests.cpp
        tests/persistence_tests.cpp
        tests/playback_tests.cpp
        tests/preferences_tests.cpp
        tests/real_bank_tests.cpp
        tests/sample_pitch_tests.cpp
        tests/audition_tests.cpp
        tests/sf2_tests.cpp
        tests/spc700_tests.cpp
        tests/spc_file_tests.cpp
        tests/timeline_tests.cpp
        tests/ui_bundle_tests.cpp
    )

    target_link_libraries(midi_composer_tests
        PRIVATE
            midi_composer_core
            doctest::doctest
            clap_headers      # the fake plugin is written against the real ABI
            miniz          # the bundle tests write real archives to read back
    )

    # doctest forward-declares std::ostream by default to cut compile time, which
    # leaves it incomplete for MSVC's <string_view> stream operators.
    target_compile_definitions(midi_composer_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)

    # The core links Saucer, whose headers trip the project's warning level; the
    # tests exercise the domain layers, so they use default warnings.
    add_test(NAME midi_composer_tests COMMAND midi_composer_tests)
endif()

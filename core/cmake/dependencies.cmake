include(FetchContent)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# Saucer
FetchContent_Declare(
    saucer
    GIT_REPOSITORY https://github.com/saucer/saucer.git
    GIT_TAG v2.0.0
)
FetchContent_MakeAvailable(saucer)

# libremidi
FetchContent_Declare(
    libremidi
    GIT_REPOSITORY https://github.com/jcelerier/libremidi.git
    GIT_TAG v4.5.0
)
FetchContent_MakeAvailable(libremidi)

# miniz — reads the packed UI archive at startup. Chosen over zlib because it is
# one translation unit with no system dependency and it understands the zip
# container itself, so the archive is a plain zip any tool can open.
#
# 3.1.2 and not 3.0.2: the older release declares cmake_minimum_required(3.0),
# which CMake 4 refuses outright. CMake 3.x only warns about it, so this is
# invisible in a local build and fails the moment CI runs a current CMake.
#
# Its examples, tests and install rules are already off when it is not the
# top-level project, so there is nothing to force here — and forcing generically
# named options like BUILD_TESTS into the cache reaches every other fetched
# project too.
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.1.2
)
FetchContent_MakeAvailable(miniz)

# doctest — only fetched when tests are enabled, so a normal build is unaffected.
if(MIDI_COMPOSER_BUILD_TESTS)
    # 2.4.11 declares cmake_minimum_required(3.0), which CMake 4 refuses.
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
    )
    FetchContent_MakeAvailable(doctest)
endif()

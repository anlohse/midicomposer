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
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
set(BUILD_HEADER_ONLY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz)

# doctest — only fetched when tests are enabled, so a normal build is unaffected.
if(MIDI_COMPOSER_BUILD_TESTS)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    FetchContent_MakeAvailable(doctest)
endif()

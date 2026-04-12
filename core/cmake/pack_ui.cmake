# Builds the UI and packs it into the zip the app loads at startup.
#
# Everything is done here, in one script invoked with -P, rather than as a
# sequence of COMMANDs on the custom target. On Windows the Visual Studio
# generator writes those commands into a single batch file, and running npm.cmd
# from a batch file without `call` abandons the rest of it — the pack step is
# silently skipped and the build still succeeds, leaving no archive behind.
# One command cannot be half-run.
#
# Run at build time, not configure time: the bundle filename carries a content
# hash, so the file list is only known once the UI has been built.
#
# Expects: UI_DIR (the ui project), UI_PAK (archive to write), NPM.
# Runnable by hand:
#   cmake -DUI_DIR=... -DUI_PAK=... -DNPM=npm -P cmake/pack_ui.cmake

foreach(required UI_DIR UI_PAK NPM)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "pack_ui.cmake requires -D${required}=<value>")
    endif()
endforeach()

set(ui_dist "${UI_DIR}/dist")

# Parcel writes its dev-server output to dist/ as well, so without this a stale
# dev bundle gets packed next to the real one — and index.html points at
# whichever was written last.
file(REMOVE_RECURSE "${ui_dist}")

execute_process(
    COMMAND "${NPM}" run build
    WORKING_DIRECTORY "${UI_DIR}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "The UI build failed (${build_result})")
endif()

if(NOT EXISTS "${ui_dist}/index.html")
    message(FATAL_ERROR "No index.html in ${ui_dist}; the UI build produced no bundle")
endif()

# ── Inline the script into the page ──────────────────────────────────────────
#
# Not an optimisation. The app serves the bundle over saucer's custom scheme,
# whose origin the webview treats as opaque, and a <script type="module"> is
# always fetched with CORS. The scheme handler sends no Access-Control-Allow-
# Origin, so a linked module is blocked outright: the page loads, the script
# never runs, and the window comes up blank with the failure only visible in the
# dev tools console. An inline script is not fetched at all, so the question
# never arises.
file(READ "${ui_dist}/index.html" html)

set(script_pattern "<script[^>]*src=[\"']?([^\"'> ]+)[\"']?[^>]*>[ \t\r\n]*</script>")
string(REGEX MATCH "${script_pattern}" script_tag "${html}")
if(NOT script_tag)
    message(FATAL_ERROR
        "No <script src=...> found in ${ui_dist}/index.html. The bundler's output "
        "changed shape; inlining has to be revisited before this can ship.")
endif()
set(script_file "${CMAKE_MATCH_1}")

if(NOT EXISTS "${ui_dist}/${script_file}")
    message(FATAL_ERROR "index.html references ${script_file}, which was not built")
endif()

file(READ "${ui_dist}/${script_file}" script_body)
# A "</script" anywhere in the code — inside a string literal, say — would end
# the tag early. Escaping the slash leaves the JavaScript meaning untouched.
string(REPLACE "</script" "<\\/script" script_body "${script_body}")
string(REPLACE "${script_tag}" "<script type=\"module\">${script_body}</script>" html "${html}")
file(WRITE "${ui_dist}/index.html" "${html}")

file(GLOB_RECURSE ui_files RELATIVE "${ui_dist}" "${ui_dist}/*")
list(REMOVE_ITEM ui_files "${script_file}")

# Anything else the page would have to fetch is subject to the same CORS rule.
# Fail rather than ship a build that dies at startup: this fires the day the
# bundler starts code-splitting, which is exactly when it is easy to miss.
set(remaining_scripts ${ui_files})
list(FILTER remaining_scripts INCLUDE REGEX "\\.(js|mjs)$")
list(FILTER remaining_scripts EXCLUDE REGEX "\\.map$")
if(remaining_scripts)
    message(FATAL_ERROR
        "The UI build produced more than one script (${remaining_scripts}). "
        "Only the entry bundle is inlined, and a fetched one is blocked by CORS "
        "on the custom scheme, so this would ship a blank window.")
endif()

# Source maps are several times the size of the code they describe and are of no
# use in a shipped build. Dropped here rather than by asking the bundler not to
# emit them, so they stay on disk for debugging a production build.
list(FILTER ui_files EXCLUDE REGEX "\\.map$")

if(NOT ui_files)
    message(FATAL_ERROR "Nothing to pack in ${ui_dist}")
endif()

# The pak target can run before the executable has ever been linked, so the
# output directory is not there yet in a fresh build tree.
get_filename_component(pak_dir "${UI_PAK}" DIRECTORY)
file(MAKE_DIRECTORY "${pak_dir}")

# Written to a temporary and moved into place, so an interrupted pack cannot
# leave a truncated archive that the app would then fail to open.
set(staging "${UI_PAK}.tmp")
file(REMOVE "${staging}")

# Through `tar` with a working directory rather than file(ARCHIVE_CREATE):
# entries have to be named relative to the dist root, because those names are
# the URL paths the webview asks for.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${staging}" --format=zip ${ui_files}
    WORKING_DIRECTORY "${ui_dist}"
    RESULT_VARIABLE pack_result
    ERROR_VARIABLE pack_error
)
if(NOT pack_result EQUAL 0)
    message(FATAL_ERROR "Packing the UI failed: ${pack_error}")
endif()

file(RENAME "${staging}" "${UI_PAK}")

list(LENGTH ui_files count)
file(SIZE "${UI_PAK}" packed_size)
message(STATUS "Packed ${count} UI files into ${UI_PAK} (${packed_size} bytes)")

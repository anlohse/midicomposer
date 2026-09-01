#pragma once

namespace midi_composer::shell {

/**
 * Makes the process say why it died.
 *
 * A `catch (...)` only sees C++ exceptions. An access violation, a stack
 * overflow or a call to `std::terminate` walk straight past it, and on Windows
 * the process then disappears with no window, no message and nothing in the
 * log -- which is exactly the report that cannot be acted on.
 *
 * Installs three things, each covering a way out that the others miss:
 *
 *   - an unhandled-exception filter, for structured exceptions: the code and
 *     the address, which say *what* rather than merely *that*;
 *   - a terminate handler, for an exception nobody caught or a failed noexcept;
 *   - an exit hook, so a clean exit leaves a line too. That last one matters
 *     more than it sounds: without it, "the log stops here" cannot distinguish
 *     a crash from an orderly shutdown nobody asked for, and those want
 *     completely different fixes.
 *
 * Nothing here changes behaviour. It only means the next report is evidence.
 */
void install_crash_reporting();

/**
 * Puts our unhandled-exception filter back if something replaced it.
 *
 * There is only one such filter per process, and the last caller wins. The
 * WebView2 runtime brings its own crash handler along, which is why a fault
 * during startup could reach Windows Error Reporting while leaving our log
 * ending mid-sentence. `after` names what we just did, for the log line.
 */
void reassert_crash_reporting(const char* after);

/** Faults on purpose when MC_TEST_CRASH names this moment. A crash reporter
    that has never reported a crash is a guess; this is how it gets tested. */
void test_crash_if_asked(const char* moment);

/** Where the process is, for a report: elevation, integrity and session. Some
    failures only happen on one side of those. */
void log_process_context();

} // namespace midi_composer::shell

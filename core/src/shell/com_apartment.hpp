#pragma once

namespace midi_composer::shell {

/**
 * Declares the main thread a single-threaded apartment, before anything else.
 *
 * On Windows the first library to call CoInitializeEx decides what kind of
 * apartment a thread lives in, and every later caller is told "no" -- so an
 * apartment nobody claims on purpose is claimed by accident, by whichever
 * dependency happens to run first.
 *
 * That is not a stylistic matter here. The webview must be created on a
 * single-threaded apartment; the audio backend asks for a multithreaded one
 * for whatever thread starts it. Once startup began restoring a saved output,
 * the audio device started before the window existed, and on a machine where
 * nothing else had claimed the thread first the webview could no longer be
 * created. Worse, it failed asynchronously: the environment callback arrived
 * carrying RPC_E_CHANGED_MODE and a null environment, which saucer
 * dereferences -- so the process died with an access violation and no window,
 * no message and nothing in the log.
 *
 * Claiming it here settles the question before any dependency can. The audio
 * backend's own request then fails harmlessly, which is what it already
 * expects on a thread that belongs to someone else.
 *
 * Returns false only if something got there first and chose otherwise -- which
 * cannot happen from our own code, but can from a DLL injected into the
 * process. The caller should say so and stop, rather than crash later.
 *
 * No matching CoUninitialize: the apartment has to outlive everything in the
 * process that uses COM, which includes the webview being torn down.
 */
[[nodiscard]] bool initialize_ui_apartment();

/** What apartment the calling thread is in, as text. */
const char* current_apartment();

} // namespace midi_composer::shell

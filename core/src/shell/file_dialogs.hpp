#pragma once

#include <optional>
#include <string>

namespace midi_composer::shell {

// Native modal file dialogs. Returned paths are UTF-8; nullopt means the
// user cancelled. `filter` uses the Win32 double-NUL convention, e.g.
// L"MIDI Files (*.mid)\0*.mid\0All Files (*.*)\0*.*\0".
std::optional<std::string> open_file_dialog(const wchar_t* filter);
std::optional<std::string> save_file_dialog(const wchar_t* filter, const wchar_t* default_extension);

// Picks a directory rather than a file. Used for the folders scanned for
// plugins, where what is wanted is the folder itself and not anything in it.
std::optional<std::string> open_folder_dialog();

// Builds a filter for open_file_dialog out of a pattern a plugin declared for
// one of its File parameters: "*.spc", or several separated by semicolons.
// Whitespace around a piece is trimmed and empty pieces are dropped.
//
// All Files is always the last entry, and the only one when nothing usable was
// declared: a filter that hides the file the user came to pick is worse than
// one that shows too much.
std::wstring make_file_filter(const std::string& pattern);

// Reports a failure that stops the app from starting. Startup errors happen
// before there is a window to show them in, and a user who double-clicked the
// executable would otherwise see nothing at all.
void show_error_dialog(const std::string& title, const std::string& message);

} // namespace midi_composer::shell

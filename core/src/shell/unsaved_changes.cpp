#include "unsaved_changes.hpp"

#include "app/core_facade.hpp"
#include "base/logger.hpp"
#include "shell/file_dialogs.hpp"

#include <string>
#include <vector>

namespace midi_composer::shell {

namespace {

constexpr const wchar_t* kProjectFilter =
    L"MIDI Composer Project (*.mcproj)\0*.mcproj\0All Files (*.*)\0*.*\0";

/** Saves one document, asking where to put it if it has never been saved.
    False means it is still unsaved -- a cancelled dialog or a failed write. */
bool save_one(app::CoreFacade& core, base::CompositionId id, const std::string& title) {
    std::string path = core.get_project_path(id);
    if (path.empty()) {
        auto chosen = save_file_dialog(kProjectFilter, L"mcproj");
        if (!chosen) return false;          // the person changed their mind
        path = *chosen;
    }
    if (auto saved = core.save_project(id, path); !saved) {
        show_error_dialog("MIDI Composer",
                          "\"" + title + "\" could not be saved:\n\n" +
                              saved.error().message +
                              "\n\nNothing has been closed.");
        return false;
    }
    return true;
}

} // namespace

bool resolve_unsaved(app::CoreFacade& core, base::CompositionId id) {
    if (!core.has_unsaved_changes(id)) return true;

    const auto title = core.document_title(id);
    switch (ask_unsaved_changes(title)) {
        case UnsavedChoice::Discard: return true;
        case UnsavedChoice::Cancel:  return false;
        case UnsavedChoice::Save:    return save_one(core, id, title);
    }
    return false;
}

bool resolve_unsaved_all(app::CoreFacade& core) {
    const auto pending = core.unsaved_documents();
    if (pending.empty()) return true;

    // One question for the lot. Asked per document, closing five tabs would
    // mean five dialogs to answer before finding out the last one cancels.
    const std::string what = pending.size() == 1
        ? core.document_title(pending.front())
        : std::to_string(pending.size()) + " documents";

    switch (ask_unsaved_changes(what)) {
        case UnsavedChoice::Discard:
            return true;
        case UnsavedChoice::Cancel:
            return false;
        case UnsavedChoice::Save:
            for (const auto id : pending) {
                if (!save_one(core, id, core.document_title(id))) {
                    MC_LOG_INFO("Closing was stopped: {} is still unsaved",
                                core.document_title(id));
                    return false;
                }
            }
            return true;
    }
    return false;
}

} // namespace midi_composer::shell

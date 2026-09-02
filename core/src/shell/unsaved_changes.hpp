#pragma once

#include "base/strong_id.hpp"

namespace midi_composer::app { class CoreFacade; }

namespace midi_composer::shell {

/**
 * Stands between unsaved work and the thing that would discard it.
 *
 * A document knows it is dirty and the window shows an asterisk for it, and
 * until this existed nothing acted on either: closing a tab dropped the
 * document, closing the window dropped all of them, and neither asked. Silent
 * loss of work is the one fault a person does not forgive, and it was the one
 * item on the MVP's own verification checklist -- "close the app cleanly
 * without corrupting work" -- that did not hold.
 *
 * Both entry points come through here so there is one behaviour and one place
 * to read it. The dialog is native rather than the application's own: it is
 * asked while the window is being taken away, which is the worst moment to need
 * a round trip through the webview.
 */

/**
 * Asks about one document and acts on the answer.
 *
 * Returns false when the answer was to cancel, which means: do not close.
 * A clean document is never asked about, so the common case costs nothing.
 */
[[nodiscard]] bool resolve_unsaved(app::CoreFacade& core, base::CompositionId id);

/**
 * The same for every open document, asked once rather than once per tab.
 *
 * Returns false when the answer was to cancel. Saving stops at the first
 * document that cannot be saved -- a cancelled file dialog included -- and
 * reports failure, because carrying on would close the ones behind it.
 */
[[nodiscard]] bool resolve_unsaved_all(app::CoreFacade& core);

} // namespace midi_composer::shell

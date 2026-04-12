#include "application.hpp"
#include "base/logger.hpp"
#include "shell/file_dialogs.hpp"
#include "shell/ui_bundle.hpp"
#include "ui_bridge/bridge_dispatcher.hpp"
#include <saucer/smartview.hpp>
#include <saucer/serializers/glaze/glaze.hpp>
#include <memory>

int main() {
    midi_composer::shell::Application app_context;
    app_context.initialize();
    app_context.core().initialize();

    saucer::smartview view;

    view.on<saucer::window_event::close>([&app_context]() -> bool {
        app_context.core().shutdown();
        return false; // false = allow the window to close
    });

    midi_composer::ui_bridge::BridgeDispatcher dispatcher(app_context.core());
    dispatcher.register_with(view);

    app_context.core().playback_engine().set_position_callback([&dispatcher](midi_composer::timeline::Tick tick) {
        nlohmann::json payload;
        payload["tick"] = tick.value();
        dispatcher.send_notification("playback_position", payload);
    });

    // Transport transitions, including the automatic stop when playback runs
    // past the last note. Without this the UI only learns about that stop from
    // its one-second poll, so the buttons lie in the meantime.
    app_context.core().playback_engine().set_state_callback(
        [&dispatcher](midi_composer::playback::TransportState state,
                      midi_composer::timeline::Tick tick) {
            nlohmann::json payload;
            payload["state"] = midi_composer::playback::transport_state_name(state);
            payload["tick"] = tick.value();
            dispatcher.send_notification("transport_state", payload);
        });

    // Pushed after every committed document mutation — whether it came from a UI
    // command, undo/redo, or a recorded note. The UI applies the listed changes
    // to its mirror; only `resync: true` patches make it re-fetch a snapshot.
    app_context.core().set_document_patched_callback([&dispatcher](const nlohmann::json& patch) {
        dispatcher.send_notification("document_patched", patch);
    });

    view.set_title("MIDI Composer");
    view.set_size(1280, 720);

    // Force a variable on window to confirm native context
    view.inject("window.is_native_host = true;", saucer::load_time::creation);

    // Where the UI comes from is the one real difference between a development
    // build and a shipped one.
#ifdef MIDI_COMPOSER_DEV_WEB_UI
    // Parcel's dev server, for hot rebuilds. Dev tools come along with it.
    view.set_dev_tools(true);
    view.set_url("http://localhost:1234");
#else
    // Shipped: the bundle packed next to the executable, served over saucer's
    // embedded-file scheme. No dev server, no loopback socket, no port to
    // collide with — and nothing on disk for a user to edit out from under us.
    //
    // `bundle` has to outlive view.run(): saucer keeps spans into its bytes.
    auto bundle = midi_composer::shell::UiBundle::load(
        midi_composer::shell::default_ui_bundle_path());
    if (!bundle) {
        MC_LOG_ERROR("{}", bundle.error().message);
        midi_composer::shell::show_error_dialog(
            "MIDI Composer",
            bundle.error().message +
                "\n\nThe application cannot start without its UI bundle. "
                "Reinstall, or rebuild with the ui.pak target.");
        app_context.core().shutdown();
        return 1;
    }
    view.embed((*bundle)->embedded());
    view.serve("index.html");
#endif

    view.show();
    view.run();

    // The webview is dead once run() returns; stop routing events to it
    // before shutdown paths (playback stop) try to notify the UI.
    dispatcher.detach();
    app_context.shutdown();

    return 0;
}

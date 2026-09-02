#include "application.hpp"
#include "app/preferences.hpp"
#include "base/logger.hpp"
#include "shell/com_apartment.hpp"
#include "shell/crash_report.hpp"
#include "shell/unsaved_changes.hpp"
#include "shell/file_dialogs.hpp"
#include "shell/ui_bundle.hpp"
#include "ui_bridge/bridge_dispatcher.hpp"
#include <saucer/smartview.hpp>
#include <saucer/serializers/glaze/glaze.hpp>
#include <memory>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace {

/**
 * What the webview is about to be created with.
 *
 * Creating it depends on more of the machine than any other step: a runtime
 * installed outside this process, a data folder somewhere in the profile, and
 * four environment variables that redirect all of it. When it fails on one
 * machine and not another, this is the difference, and guessing at it from the
 * outside costs a round trip each time.
 */
void log_webview_environment() {
    for (const char* name : {"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
                             "WEBVIEW2_USER_DATA_FOLDER",
                             "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                             "WEBVIEW2_RELEASE_CHANNEL_PREFERENCE",
                             "APPDATA", "TEMP"}) {
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, name) == 0 && value) {
            MC_LOG_INFO("  {} = {}", name, value);
            std::free(value);
        } else {
            MC_LOG_INFO("  {} is not set", name);
        }
    }
    MC_LOG_INFO("  WebView2 runtime = {}", midi_composer::shell::webview2_runtime_version());
}

/**
 * The folder the webview will keep its profile in, created and checked first.
 *
 * Creating the webview fails if this folder cannot be written, and it fails
 * through an asynchronous callback that hands back a null environment which
 * saucer dereferences without checking -- so the process dies with an access
 * violation and no explanation of the actual problem. Testing the folder here
 * turns that into a sentence.
 *
 * An empty return means "no opinion": the library falls back to its own
 * choice, which is better than refusing to start over a directory.
 */
std::filesystem::path prepared_storage_path() {
    const auto folder = midi_composer::app::Preferences::webview_storage_folder();
    if (folder.empty()) return {};

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        MC_LOG_WARN("Could not create the webview folder {}: {}",
                    folder.string(), ec.message());
        return {};
    }
    MC_LOG_INFO("  webview profile = {}", folder.string());
    return folder;
}

/**
 * Reports a startup failure instead of vanishing.
 *
 * Everything below runs before there is a window, so anything that throws here
 * ends the process with no window, no message, and -- until the log became a
 * file -- no trace at all. That is the worst failure an application can have,
 * because the person in front of it has nothing to report but "it does not
 * start".
 */
[[noreturn]] void die(const std::string& stage, const std::string& detail) {
    MC_LOG_CRITICAL("Failed during {}: {}", stage, detail);
    midi_composer::shell::show_error_dialog(
        "MIDI Composer",
        "The application could not start.\n\nStage: " + stage + "\n" + detail +
            "\n\nThe full log is in:\n" +
            midi_composer::base::Logger::log_path().string());
    std::exit(1);
}

} // namespace

int main() {
    midi_composer::shell::Application app_context;
    app_context.initialize();
    // Before anything that can fail, so a fault during startup is described
    // rather than silent.
    midi_composer::shell::install_crash_reporting();
    midi_composer::shell::log_process_context();

    // Before the core, because starting the audio device claims the main
    // thread for a multithreaded apartment if nobody claimed it first -- and
    // the webview cannot be created on one of those.
    if (!midi_composer::shell::initialize_ui_apartment()) {
        die("claiming the main thread for the user interface",
            "Something in this process put the main thread into a multithreaded "
            "apartment before the application started.");
    }

    app_context.core().initialize();

    // The webview is created here, and creating it is the step most likely to
    // fail on a machine rather than in the code: it needs the WebView2 runtime
    // present, and it needs a profile folder it can write. When that folder is
    // unusable the failure comes back through a callback carrying a null
    // environment, and saucer dereferences it -- so the whole process dies at
    // this line with an access violation. Hence the log lines around it.
    log_webview_environment();
    MC_LOG_INFO("  main thread apartment = {}", midi_composer::shell::current_apartment());
    const auto storage_path = prepared_storage_path();
    MC_LOG_INFO("Creating the webview");
    std::unique_ptr<saucer::smartview<>> view_holder;
    try {
        view_holder = std::make_unique<saucer::smartview<>>(
            saucer::options{.storage_path = storage_path});
    } catch (const std::exception& e) {
        die("creating the webview", e.what());
    } catch (...) {
        die("creating the webview",
            "The WebView2 runtime is missing, or another instance still holds its data folder.");
    }
    auto& view = *view_holder;
    MC_LOG_INFO("Webview created");
    // The WebView2 runtime is loaded now, and it brings its own crash handler.
    midi_composer::shell::reassert_crash_reporting("the WebView2 runtime");
    midi_composer::shell::test_crash_if_asked("after-webview");

    view.on<saucer::window_event::close>([&app_context, &view]() -> bool {
        // Returning true keeps the window. Work that is not on disk is the one
        // thing worth stopping a close for, and this is the last moment anyone
        // can be asked about it.
        if (!midi_composer::shell::resolve_unsaved_all(app_context.core())) return true;

        // The size is read here rather than on every resize: this is the only
        // moment it is certainly final, and a preferences write per drag of a
        // window edge would be a file written hundreds of times for nothing.
        //
        // Its own size while maximized is the restored size, which is what
        // should come back when the window is un-maximized later -- so both are
        // kept and neither replaces the other.
        const auto [width, height] = view.size();
        if (auto saved = app_context.core().remember_window(width, height, view.maximized());
            !saved) {
            MC_LOG_WARN("Could not remember the window size: {}", saved.error().message);
        }

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

    // The size from last time, or a sensible default on a first run. Validated
    // in the preferences, so what arrives here is either usable or nothing.
    {
        const auto& prefs = app_context.core().preferences();
        if (prefs.window_width() > 0 && prefs.window_height() > 0) {
            view.set_size(prefs.window_width(), prefs.window_height());
        } else {
            view.set_size(1280, 720);
        }
        // After the size, so un-maximizing later lands on the size above
        // rather than on whatever the window happened to be first.
        if (prefs.window_maximized()) view.set_maximized(true);
    }

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

    MC_LOG_INFO("Showing the window");
    view.show();
    view.run();

    // The webview is dead once run() returns; stop routing events to it
    // before shutdown paths (playback stop) try to notify the UI.
    dispatcher.detach();
    app_context.shutdown();

    return 0;
}

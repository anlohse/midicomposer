#pragma once

#include "base/error.hpp"
#include "playback/output_plugin.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace midi_composer::app {

/**
 * What this installation remembers between runs, as opposed to what a project
 * remembers.
 *
 * ── Why these settings and not others ────────────────────────────────────────
 *
 * §10.2 asked whether the chosen output belongs to the project or to the
 * machine, and this is the answer: to the machine. A MIDI port is a property of
 * the computer it is plugged into, so opening someone else's project must not
 * silently repoint your sound at a device they happen to own. The composition
 * stays a composition.
 *
 * The counter-argument in §10.2 was that a plugin is instrument as much as
 * device, and an instrument really is the piece's. That case is not lost here:
 * a *track* names its output, and that name is saved in the project (§9a). So
 * the piece keeps its instruments and the machine keeps its devices, which is
 * the split that was actually wanted.
 *
 * ── Failing to load is not a failure to start ────────────────────────────────
 *
 * Every read is best-effort. A file that is missing, unreadable, or corrupt
 * leaves the defaults in place and the application starts anyway: preferences
 * exist to save the user a few clicks, and losing them must never cost more
 * than those clicks.
 *
 * Writes are the opposite -- they are atomic, precisely so a crash halfway
 * through cannot turn "a few clicks" into a file that will not parse again.
 */
class Preferences {
public:
    /**
     * Where preferences live on this machine, per-user and roaming.
     *
     * Empty if the platform will not say, in which case nothing is loaded or
     * saved and the application runs on defaults.
     */
    [[nodiscard]] static std::filesystem::path default_path();

    /**
     * The folder this installation owns for plugins dropped in by hand.
     *
     * Always scanned, never listed as a removable search path, and created at
     * startup so it exists to be pasted into. Somewhere to put a plugin has to
     * exist before the user is asked to put one somewhere -- "add the folder
     * you downloaded it to" only works for someone who already understands
     * that plugins live in folders.
     *
     * Local rather than roaming, unlike the preferences beside it: these are
     * native binaries, and syncing them between machines would carry a build
     * for one architecture onto another and count against a roaming quota
     * besides.
     */
    [[nodiscard]] static std::filesystem::path plugin_folder();

    /** Read a file if there is one. Absent or unparseable is not an error. */
    void load(const std::filesystem::path& path);

    /** Write to wherever load() was told to read, if anywhere. */
    base::Result<void> save() const;

    /** The output chosen last time, or empty when nothing was ever chosen. */
    [[nodiscard]] const std::string& selected_output() const { return m_selected_output; }
    void set_selected_output(std::string id) { m_selected_output = std::move(id); }

    /**
     * The parameters remembered for one output, by that output's id.
     *
     * Kept per output rather than globally because two outputs may both have a
     * "port", and they are not the same port.
     */
    [[nodiscard]] std::map<std::string, playback::ParameterValue>
    parameters_for(const std::string& output_id) const;

    void set_parameter(const std::string& output_id, const std::string& name,
                       const playback::ParameterValue& value);

    /**
     * Folders to scan for `.clap` files beyond the standard ones.
     *
     * CLAP_PATH already answers this, and an earlier note here argued that a
     * setting of our own would be a second answer to a settled question. That
     * held while there was nowhere to put the setting. There is now, and an
     * environment variable is not something a user can be asked to set: it has
     * to be exported before launch, which means it cannot be changed from
     * inside the running application at all. Both are read; neither replaces
     * the other.
     */
    [[nodiscard]] const std::vector<std::string>& clap_search_paths() const {
        return m_clap_search_paths;
    }
    void set_clap_search_paths(std::vector<std::string> paths) {
        m_clap_search_paths = std::move(paths);
    }

    /** Serialise and parse, exposed so both directions can be tested alone. */
    [[nodiscard]] std::string to_json() const;
    void from_json(const std::string& text);

private:
    std::filesystem::path m_path;
    std::string m_selected_output;
    std::map<std::string, std::map<std::string, playback::ParameterValue>> m_parameters;
    std::vector<std::string> m_clap_search_paths;
};

} // namespace midi_composer::app

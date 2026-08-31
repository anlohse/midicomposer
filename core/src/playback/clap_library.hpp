#pragma once

#include "base/error.hpp"
#include "playback/clap_instance.hpp"

#include <memory>
#include <string>
#include <vector>

namespace midi_composer::playback {

/**
 * One `.clap` file, open.
 *
 * A `.clap` is a shared library with a different extension, exporting a single
 * `clap_entry` symbol. This owns that module: the entry has to be de-initialised
 * and the library unloaded *after* the last plugin from it is destroyed, which
 * is why an instance keeps a reference to the library it came from rather than
 * the caller being trusted to get the order right.
 */
class ClapLibrary : public std::enable_shared_from_this<ClapLibrary> {
public:
    struct Descriptor {
        std::string id;       // the plugin's own id, stable across versions
        std::string name;     // displayable
        std::string vendor;
    };

    ~ClapLibrary();

    ClapLibrary(const ClapLibrary&) = delete;
    ClapLibrary& operator=(const ClapLibrary&) = delete;

    /** Open a `.clap` and read its factory. */
    static base::Result<std::shared_ptr<ClapLibrary>> open(const std::string& path);

    [[nodiscard]] const std::string& path() const { return m_path; }

    /** What this file offers. A file may contain several plugins. */
    [[nodiscard]] std::vector<Descriptor> plugins() const;

    /** Create one, initialised and ready to be started. */
    base::Result<std::unique_ptr<ClapInstance>> create(const std::string& plugin_id,
                                                       int sample_rate);

    /**
     * Every `.clap` found in the places a plugin is normally installed, plus
     * whatever CLAP_PATH names, plus whatever the caller adds.
     *
     * CLAP_PATH is the specification's own escape hatch, and for a long time
     * that was the whole answer here: a setting of our own would be a second
     * answer to a settled question. What it cannot be is *changed*. It has to
     * be exported before launch, so a user who downloads a plugin into a folder
     * of their own has no way to point a running application at it. `extra` is
     * where the preferences file puts that folder. Both are read.
     */
    [[nodiscard]] static std::vector<std::string> search_paths(
        const std::vector<std::string>& extra = {});
    [[nodiscard]] static std::vector<std::string> find_plugin_files(
        const std::vector<std::string>& extra = {});

private:
    ClapLibrary() = default;

    void* m_module{nullptr};
    const void* m_entry{nullptr};      // clap_plugin_entry_t
    const void* m_factory{nullptr};    // clap_plugin_factory_t
    std::string m_path;
};

} // namespace midi_composer::playback

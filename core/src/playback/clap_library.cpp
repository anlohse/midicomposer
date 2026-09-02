#include "clap_library.hpp"

#include "base/logger.hpp"

#include <clap/clap.h>

#include <filesystem>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace midi_composer::playback {

namespace {

const clap_plugin_entry_t* entry_of(const void* p) {
    return static_cast<const clap_plugin_entry_t*>(p);
}

const clap_plugin_factory_t* factory_of(const void* p) {
    return static_cast<const clap_plugin_factory_t*>(p);
}

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
}

std::string from_path(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::string env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || !value) return {};
    std::string out(value);
    free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

} // namespace

ClapLibrary::~ClapLibrary() {
    if (m_entry) entry_of(m_entry)->deinit();
#ifdef _WIN32
    if (m_module) FreeLibrary(static_cast<HMODULE>(m_module));
#endif
}

base::Result<std::shared_ptr<ClapLibrary>> ClapLibrary::open(const std::string& path) {
#ifndef _WIN32
    return std::unexpected(base::Error{base::ErrorCode::UnsupportedFormat,
                                       "Loading plugins is implemented for Windows only"});
#else
    auto library = std::shared_ptr<ClapLibrary>(new ClapLibrary());
    library->m_path = path;

    HMODULE module = LoadLibraryW(to_path(path).wstring().c_str());
    if (!module) {
        return std::unexpected(base::Error{
            base::ErrorCode::IoFailure,
            "Could not load '" + path + "' (error " + std::to_string(GetLastError()) + ")"});
    }
    library->m_module = module;

    // A .clap exports exactly one symbol. Anything else is not one, whatever
    // the file is called.
    const auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(
        GetProcAddress(module, "clap_entry"));
    if (!entry) {
        return std::unexpected(base::Error{base::ErrorCode::UnsupportedFormat,
                                           "'" + path + "' exports no clap_entry"});
    }
    if (!clap_version_is_compatible(entry->clap_version)) {
        return std::unexpected(base::Error{
            base::ErrorCode::UnsupportedFormat,
            "'" + path + "' was built against an incompatible CLAP version"});
    }

    // The plugin is told where it lives; some load resources relative to it.
    if (!entry->init(path.c_str())) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           "'" + path + "' failed to initialise"});
    }
    library->m_entry = entry;

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        return std::unexpected(base::Error{base::ErrorCode::UnsupportedFormat,
                                           "'" + path + "' offers no plugin factory"});
    }
    library->m_factory = factory;
    return library;
#endif
}

std::vector<ClapLibrary::Descriptor> ClapLibrary::plugins() const {
    std::vector<Descriptor> out;
    if (!m_factory) return out;
    const auto* factory = factory_of(m_factory);
    const uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* desc = factory->get_plugin_descriptor(factory, i);
        if (!desc || !desc->id) continue;
        out.push_back({desc->id,
                       desc->name ? desc->name : desc->id,
                       desc->vendor ? desc->vendor : ""});
    }
    return out;
}

base::Result<std::unique_ptr<ClapInstance>> ClapLibrary::create(const std::string& plugin_id,
                                                                int sample_rate) {
    if (!m_factory) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState, "Library not open"});
    }
    std::string name = plugin_id;
    for (const auto& d : plugins()) {
        if (d.id == plugin_id) { name = d.name; break; }
    }

    const auto* factory = factory_of(m_factory);
    // A ClapInstance builds its own clap_host, so it has to exist before the
    // plugin does; the host pointer it hands over stays valid for the
    // instance's whole life, which is what the ABI requires.
    auto instance = std::make_unique<ClapInstance>(nullptr, plugin_id, name);
    const auto* plugin = factory->create_plugin(factory, instance->host(), plugin_id.c_str());
    if (!plugin) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "'" + plugin_id + "' is not in " + m_path});
    }
    instance->adopt(plugin, shared_from_this());
    instance->set_sample_rate(sample_rate);
    if (auto ready = instance->initialise(); !ready) {
        return std::unexpected(ready.error());
    }
    return instance;
}

std::vector<std::string> ClapLibrary::search_paths(const std::vector<std::string>& extra) {
    std::vector<std::string> paths;

    // Where a plugin is normally installed on this platform.
    if (auto common = env("CommonProgramFiles"); !common.empty()) {
        paths.push_back(common + "\\CLAP");
    }
    if (auto local = env("LOCALAPPDATA"); !local.empty()) {
        paths.push_back(local + "\\Programs\\Common\\CLAP");
    }

    // And the specification's own escape hatch, semicolon separated here.
    const std::string clap_path = env("CLAP_PATH");
    size_t start = 0;
    while (start < clap_path.size()) {
        const size_t sep = clap_path.find(';', start);
        auto piece = clap_path.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!piece.empty()) paths.push_back(piece);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }

    // Last, so a folder the user added cannot hide a plugin that is properly
    // installed: the first file wins when the same plugin is found twice.
    for (const auto& dir : extra) {
        if (!dir.empty()) paths.push_back(dir);
    }
    return paths;
}

std::vector<std::string> ClapLibrary::find_plugin_files(const std::vector<std::string>& extra) {
    std::vector<std::string> files;
    for (const auto& dir : search_paths(extra)) {
        std::error_code ec;
        const auto root = to_path(dir);

        // A folder that is not there is the normal case: most machines have no
        // CLAP_PATH and no Common Files\CLAP, and saying so every launch would
        // be noise.
        if (!std::filesystem::exists(root, ec) || ec) {
            if (ec) MC_LOG_WARN("Cannot look at the plugin folder {}: {}", dir, ec.message());
            continue;
        }
        if (!std::filesystem::is_directory(root, ec) || ec) {
            if (ec) MC_LOG_WARN("Cannot look at the plugin folder {}: {}", dir, ec.message());
            continue;
        }

        // A folder that is there and cannot be read is a different thing, and
        // it used to look identical: every error_code below was dropped on the
        // floor, so an unreadable folder and an empty one produced the same
        // empty list and the same silence. That is how a plugin folder can
        // stop working with nothing anywhere saying so -- found while packaging
        // this application as MSIX, where exactly that happened and the log had
        // nothing to offer.
        // Listed once without skip_permission_denied, purely to find out
        // whether it can be listed at all.
        //
        // That flag is what made the first version of this check useless: it
        // turns a denial into a non-event, so `ec` stays clear and an
        // unreadable folder still comes back as an empty one. Which is right
        // for a *sub*folder somebody else owns, and wrong for the folder the
        // application told the user to paste plugins into. So the root is
        // probed strictly and the recursion below stays lenient.
        std::filesystem::directory_iterator probe(root, ec);
        if (ec) {
            MC_LOG_WARN("The plugin folder {} cannot be read, so nothing in it "
                        "will be found: {}", dir, ec.message());
            continue;
        }

        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            MC_LOG_WARN("Cannot walk the plugin folder {}: {}", dir, ec.message());
            continue;
        }

        size_t found = 0;
        // Recursive: plugins are sometimes a folder with the library inside.
        for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                // Stopped part way, so what was found is a partial answer and
                // the person deserves to know which folder gave up.
                MC_LOG_WARN("Stopped reading the plugin folder {} after {} plugins: {}",
                            dir, found, ec.message());
                break;
            }
            // One unreadable entry is not worth a line of its own; the folder
            // failing is.
            if (!it->is_regular_file(ec) || ec) continue;
            if (it->path().extension() == ".clap") {
                files.push_back(from_path(it->path()));
                ++found;
            }
        }
        MC_LOG_DEBUG("Plugin folder {}: {} found", dir, found);
    }
    return files;
}

} // namespace midi_composer::playback

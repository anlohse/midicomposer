#include "preferences.hpp"

#include "base/logger.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace midi_composer::app {

namespace {

// Bumped only when an older file would be *misread* rather than merely
// incomplete. Unknown keys are ignored and missing ones keep their default, so
// most additions need no bump at all.
constexpr int kSchemaVersion = 1;

// What makes the file recognisable as ours rather than merely as valid JSON.
// preferences.json is a name a dozen programs use, and the path it sits at is
// not proof of anything: a sync tool, a backup restore or a hand edit can put
// somebody else's file there. Without this, such a file would be read as a
// MIDI Composer file with every field missing -- which parses cleanly and is
// indistinguishable from a first run.
constexpr const char* kApplicationKey = "application";
constexpr const char* kApplicationName = "MIDI Composer";

std::string env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
}

std::string from_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.c_str()), text.size());
}

/** A parameter value as JSON, keeping the variant's alternative distinguishable. */
nlohmann::json value_to_json(const playback::ParameterValue& value) {
    if (const auto* flag = std::get_if<bool>(&value)) return *flag;
    if (const auto* number = std::get_if<int>(&value)) return *number;
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    return nullptr;
}

/** The inverse. An unrecognised shape reads as "not set" rather than throwing. */
playback::ParameterValue value_from_json(const nlohmann::json& node) {
    // Booleans first: nlohmann keeps them apart from numbers, but only if they
    // are asked about in that order.
    if (node.is_boolean()) return node.get<bool>();
    if (node.is_number_integer()) return node.get<int>();
    if (node.is_string()) return node.get<std::string>();
    return std::monostate{};
}

} // namespace

std::filesystem::path Preferences::default_path() {
#ifdef _WIN32
    // Roaming: the settings are the user's, not the machine's, and following a
    // profile between machines is the behaviour a user expects of them.
    const auto appdata = env("APPDATA");
    if (appdata.empty()) return {};
    return to_path(appdata) / "MIDI Composer" / "preferences.json";
#else
    const auto config = env("XDG_CONFIG_HOME");
    if (!config.empty()) return to_path(config) / "midi-composer" / "preferences.json";
    const auto home = env("HOME");
    if (home.empty()) return {};
    return to_path(home) / ".config" / "midi-composer" / "preferences.json";
#endif
}

std::filesystem::path Preferences::plugin_folder() {
#ifdef _WIN32
    const auto local = env("LOCALAPPDATA");
    if (local.empty()) return {};
    return to_path(local) / "MIDI Composer" / "Plugins";
#else
    const auto data = env("XDG_DATA_HOME");
    if (!data.empty()) return to_path(data) / "midi-composer" / "plugins";
    const auto home = env("HOME");
    if (home.empty()) return {};
    return to_path(home) / ".local" / "share" / "midi-composer" / "plugins";
#endif
}

std::filesystem::path Preferences::webview_storage_folder() {
#ifdef _WIN32
    const auto local = env("LOCALAPPDATA");
    if (local.empty()) return {};
    return to_path(local) / "MIDI Composer" / "Webview";
#else
    const auto cache = env("XDG_CACHE_HOME");
    if (!cache.empty()) return to_path(cache) / "midi-composer" / "webview";
    const auto home = env("HOME");
    if (home.empty()) return {};
    return to_path(home) / ".cache" / "midi-composer" / "webview";
#endif
}

void Preferences::load(const std::filesystem::path& path) {
    m_path = path;
    if (m_path.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(m_path, ec)) {
        // A first run, which is the normal case exactly once. Not a warning:
        // nothing went wrong.
        MC_LOG_INFO("No preferences file yet at {}", from_path(m_path));
        return;
    }

    std::ifstream file(m_path, std::ios::binary);
    if (!file) {
        MC_LOG_WARN("Could not read preferences at {}; using defaults", from_path(m_path));
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    from_json(text);
}

void Preferences::from_json(const std::string& text) {
    const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        MC_LOG_WARN("Preferences are not valid JSON; using defaults");
        return;
    }
    // Absent is accepted and rewritten on the next save: the marker was added
    // after the file was, so a file without one may well be ours. A marker that
    // is present and says something else is somebody else's file, and reading
    // it would mean adopting their settings as ours.
    if (const auto it = parsed.find(kApplicationKey); it != parsed.end()) {
        if (!it->is_string() || it->get<std::string>() != kApplicationName) {
            MC_LOG_WARN("Preferences belong to another application; using defaults");
            return;
        }
    }

    if (parsed.value("schemaVersion", kSchemaVersion) > kSchemaVersion) {
        // Written by a newer build. Reading it would mean guessing at fields
        // this one does not know, and guessing wrong then saves the guess back
        // over the real thing.
        MC_LOG_WARN("Preferences were written by a newer version; using defaults");
        return;
    }

    m_selected_output = parsed.value("selectedOutput", std::string{});
    m_metronome_output = parsed.value("metronomeOutput", std::string{});

    m_parameters.clear();
    if (const auto it = parsed.find("outputParameters");
        it != parsed.end() && it->is_object()) {
        for (const auto& [output_id, values] : it->items()) {
            if (!values.is_object()) continue;
            for (const auto& [name, value] : values.items()) {
                auto parameter = value_from_json(value);
                if (std::holds_alternative<std::monostate>(parameter)) continue;
                m_parameters[output_id][name] = std::move(parameter);
            }
        }
    }

    m_clap_search_paths.clear();
    if (const auto it = parsed.find("clapSearchPaths");
        it != parsed.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (entry.is_string()) m_clap_search_paths.push_back(entry.get<std::string>());
        }
    }
}

std::string Preferences::to_json() const {
    nlohmann::json out;
    out[kApplicationKey] = kApplicationName;
    out["schemaVersion"] = kSchemaVersion;
    out["selectedOutput"] = m_selected_output;
    out["metronomeOutput"] = m_metronome_output;

    auto parameters = nlohmann::json::object();
    for (const auto& [output_id, values] : m_parameters) {
        auto entry = nlohmann::json::object();
        for (const auto& [name, value] : values) {
            auto encoded = value_to_json(value);
            if (encoded.is_null()) continue;
            entry[name] = std::move(encoded);
        }
        if (!entry.empty()) parameters[output_id] = std::move(entry);
    }
    out["outputParameters"] = std::move(parameters);
    out["clapSearchPaths"] = m_clap_search_paths;

    // Indented: this is a file a user may well open to see what it is doing to
    // their machine, and one long line answers that badly.
    return out.dump(2);
}

base::Result<void> Preferences::save() const {
    if (m_path.empty()) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState,
                                           "No location for preferences on this platform"});
    }

    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);
    if (ec) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                           "Could not create " + from_path(m_path.parent_path())});
    }

    // Written beside the real file and moved onto it, so an interrupted write
    // leaves the previous settings intact rather than a truncated file that
    // will not parse.
    auto temporary = m_path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                               "Could not write " + from_path(temporary)});
        }
        file << to_json();
        if (!file) {
            return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                               "Could not write " + from_path(temporary)});
        }
    }

    std::filesystem::rename(temporary, m_path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                           "Could not replace " + from_path(m_path)});
    }
    return {};
}

std::map<std::string, playback::ParameterValue>
Preferences::parameters_for(const std::string& output_id) const {
    const auto it = m_parameters.find(output_id);
    return it == m_parameters.end() ? std::map<std::string, playback::ParameterValue>{}
                                    : it->second;
}

void Preferences::set_parameter(const std::string& output_id, const std::string& name,
                                const playback::ParameterValue& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        // "Not set" is the absence of a value rather than a value: storing it
        // would make the file claim a setting the plugin never reported.
        auto it = m_parameters.find(output_id);
        if (it != m_parameters.end()) it->second.erase(name);
        return;
    }
    m_parameters[output_id][name] = value;
}

} // namespace midi_composer::app

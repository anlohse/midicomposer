#pragma once

#include "base/error.hpp"
#include "music/composition.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace midi_composer::project {

// Native project persistence (versioned JSON). IDs are preserved so undo,
// selection, and references survive a save/load round trip.
class ProjectSerializer final {
public:
    static constexpr int kFormatVersion = 1;

    [[nodiscard]] static nlohmann::json to_json(const music::Composition& comp);
    [[nodiscard]] static base::Result<music::Composition> from_json(const nlohmann::json& j);

    // File helpers; `path` is UTF-8.
    static base::Result<void> save_file(const music::Composition& comp, const std::string& path);
    [[nodiscard]] static base::Result<music::Composition> load_file(const std::string& path);
};

} // namespace midi_composer::project

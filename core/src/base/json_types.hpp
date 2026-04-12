#pragma once

#include <nlohmann/json.hpp>
#include "strong_id.hpp"

namespace midi_composer::base {

template<typename Tag>
void to_json(nlohmann::json& j, const StrongId<Tag>& id) {
    j = id.value();
}

template<typename Tag>
void from_json(const nlohmann::json& j, StrongId<Tag>& id) {
    id = StrongId<Tag>{j.get<uint64_t>()};
}

} // namespace midi_composer::base

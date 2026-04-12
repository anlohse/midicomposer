#pragma once

#include <string>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <saucer/smartview.hpp>
#include <saucer/serializers/glaze/glaze.hpp>
#include "app/core_facade.hpp"

namespace midi_composer::ui_bridge {

class BridgeDispatcher {
public:
    explicit BridgeDispatcher(app::CoreFacade& core);
    ~BridgeDispatcher() = default;

    void register_with(saucer::smartview<saucer::serializers::glaze>& view);

    // Must be called once the webview is gone; evaluate() on a destroyed
    // view asserts inside saucer and takes the process down.
    void detach() { m_view = nullptr; }

    void send_notification(const std::string& type, const nlohmann::json& payload);

private:
    app::CoreFacade& m_core;
    saucer::smartview<saucer::serializers::glaze>* m_view{nullptr};
    nlohmann::json handle_command(const std::string& type, const nlohmann::json& payload);
};

} // namespace midi_composer::ui_bridge

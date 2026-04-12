#pragma once

#include <memory>
#include <string>
#include "app/core_facade.hpp"

namespace midi_composer::shell {

class Application {
public:
    Application();
    ~Application();

    void initialize();
    int run();
    void shutdown();

    [[nodiscard]] app::CoreFacade& core() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace midi_composer::shell

#include "application.hpp"
#include "base/logger.hpp"

namespace midi_composer::shell {

struct Application::Impl {
    bool initialized{false};
    std::unique_ptr<app::CoreFacade> core;
};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    m_impl->core = std::make_unique<app::CoreFacade>();
}

Application::~Application() {
    if (m_impl->initialized) {
        shutdown();
    }
}

void Application::initialize() {
    base::Logger::init();
    MC_LOG_INFO("Application initializing...");
    m_impl->initialized = true;
}

int Application::run() {
    MC_LOG_INFO("Application running...");
    return 0;
}

void Application::shutdown() {
    MC_LOG_INFO("Application shutting down...");
    m_impl->core->stop();
    m_impl->initialized = false;
}

app::CoreFacade& Application::core() const {
    return *m_impl->core;
}

} // namespace midi_composer::shell

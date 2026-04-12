#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <string>

namespace midi_composer::base {

class Logger {
public:
    static void init(const std::string& name = "MIDIComposer") {
        auto logger = spdlog::stdout_color_mt(name);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    }

    static std::shared_ptr<spdlog::logger> get() {
        return spdlog::default_logger();
    }
};

} // namespace midi_composer::base

#define MC_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MC_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define MC_LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define MC_LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define MC_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define MC_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

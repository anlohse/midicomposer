#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace midi_composer::base {

class Logger {
public:
    /**
     * Logs to a file always, and to the console when there is one.
     *
     * The file is the point. A shipped build is a windowed application with no
     * console attached, so a stdout-only logger writes into nothing -- and the
     * moment somebody says "it will not start", the only evidence there was
     * went nowhere. Now the last two runs are always on disk.
     *
     * The console sink stays for development, where reading the log as it
     * happens is the whole point, and costs nothing when no console exists.
     */
    static void init(const std::string& name = "MIDIComposer") {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        if (const auto path = log_path(); !path.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            try {
                // Two files of a megabyte: enough to cover a session and the
                // one before it, which is what "it did this yesterday too" needs.
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    path.string(), 1024 * 1024, 1));
            } catch (const spdlog::spdlog_ex&) {
                // A log that cannot be opened must not stop the application:
                // losing the diary is not losing the day.
            }
        }

        auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
        spdlog::set_default_logger(logger);
        // Debug is compiled in (see SPDLOG_ACTIVE_LEVEL in CMakeLists) but off
        // unless asked for: at debug every message crossing the bridge is
        // written, which is what you want when chasing something and noise the
        // rest of the time. MC_LOG_LEVEL=debug turns it on for a run.
        spdlog::set_level(verbose_wanted() ? spdlog::level::debug : spdlog::level::info);
        // Flushed on every line, not just on warnings. The file exists for the
        // case where somebody says "it will not start" -- and a startup that
        // ends badly ends before any buffer is written, so a buffered log of a
        // failed launch is an empty file. This is not a hot path.
        spdlog::flush_on(spdlog::level::debug);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    }

    /** Whether this run was asked for debug-level logging. */
    static bool verbose_wanted() {
#ifdef _WIN32
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, "MC_LOG_LEVEL") != 0 || !value) return false;
        const bool debug = std::string(value) == "debug";
        std::free(value);
        return debug;
#else
        const char* value = std::getenv("MC_LOG_LEVEL");
        return value && std::string(value) == "debug";
#endif
    }

    /** Where the file sink writes. Beside the plugins rather than the
        preferences: a log is machine-local and should not follow a roaming
        profile. Empty if the platform will not say. */
    static std::filesystem::path log_path() {
#ifdef _WIN32
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, "LOCALAPPDATA") != 0 || !value) return {};
        std::filesystem::path base(value);
        std::free(value);
        return base / "MIDI Composer" / "midi-composer.log";
#else
        const char* home = std::getenv("HOME");
        if (!home) return {};
        return std::filesystem::path(home) / ".local" / "state" / "midi-composer.log";
#endif
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

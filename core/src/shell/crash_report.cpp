#include "crash_report.hpp"

#include "base/logger.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <dbghelp.h>

#include <cstdlib>
#include <cstring>
#include <exception>

namespace midi_composer::shell {

namespace {

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
        case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case 0xE06D7363:                      return "an uncaught C++ exception";
        default:                              return "an unknown fault";
    }
}

/**
 * Writes the call stack to the log, named where names are known.
 *
 * "It died at 0x00000000000da28b" is not a report; it is a fact with no owner.
 * Walking the stack here turns the same crash into a list of functions and
 * lines, which is the difference between a bug someone can look at and a bug
 * someone can only reproduce.
 *
 * Frames from modules without symbols still appear, as module+offset: knowing
 * the fault happened three frames inside the WebView2 loader is worth having
 * even when the loader will not say which function.
 */
void log_stack(CONTEXT context) {
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    // Deferred loads, so a module that has no symbols costs nothing.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    MC_LOG_CRITICAL("Stack:");
    for (int depth = 0; depth < 48; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        if (address == 0) break;

        // The module first: it is the one part that is always available, and it
        // already separates "our bug" from "somebody else's".
        char module_name[MAX_PATH] = "?";
        if (const DWORD64 base = SymGetModuleBase64(process, address)) {
            char full[MAX_PATH] = {};
            if (GetModuleFileNameA(reinterpret_cast<HMODULE>(base), full, MAX_PATH)) {
                const char* slash = strrchr(full, '\\');
                strncpy_s(module_name, slash ? slash + 1 : full, _TRUNCATE);
            }
        }

        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, address, &line_displacement, &line)) {
                MC_LOG_CRITICAL("  #{:<2} {}!{} + {}   ({}:{})", depth, module_name,
                                symbol->Name, displacement, line.FileName, line.LineNumber);
            } else {
                MC_LOG_CRITICAL("  #{:<2} {}!{} + {}", depth, module_name, symbol->Name,
                                displacement);
            }
        } else {
            MC_LOG_CRITICAL("  #{:<2} {} + 0x{:X}", depth, module_name,
                            address - SymGetModuleBase64(process, address));
        }
    }
    SymCleanup(process);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    const auto* record = info && info->ExceptionRecord ? info->ExceptionRecord : nullptr;
    if (record) {
        MC_LOG_CRITICAL("Crashed: {} (0x{:08X}) at 0x{:016X}",
                        exception_name(record->ExceptionCode),
                        static_cast<unsigned>(record->ExceptionCode),
                        reinterpret_cast<uintptr_t>(record->ExceptionAddress));
        // An access violation says which address it could not touch, and
        // whether it was reading or writing. A null read is a different bug
        // from a write to freed memory.
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2) {
            MC_LOG_CRITICAL("  {} address 0x{:016X}",
                            record->ExceptionInformation[0] == 0 ? "reading" : "writing",
                            static_cast<uintptr_t>(record->ExceptionInformation[1]));
        }
    } else {
        MC_LOG_CRITICAL("Crashed, with no exception record to describe it");
    }
    if (info && info->ContextRecord) log_stack(*info->ContextRecord);
    spdlog::default_logger()->flush();
    return EXCEPTION_EXECUTE_HANDLER;   // die, but having said so
}

void on_terminate() {
    MC_LOG_CRITICAL("std::terminate was called -- an exception nobody caught, "
                    "or a throw out of something declared noexcept");
    CONTEXT context{};
    RtlCaptureContext(&context);
    log_stack(context);
    spdlog::default_logger()->flush();
    std::abort();
}

void on_exit_hook() {
    MC_LOG_INFO("Process exiting");
    spdlog::default_logger()->flush();
}

} // namespace

void install_crash_reporting() {
    SetUnhandledExceptionFilter(on_unhandled);
    std::set_terminate(on_terminate);
    std::atexit(on_exit_hook);
}

void reassert_crash_reporting(const char* after) {
    const auto previous = SetUnhandledExceptionFilter(on_unhandled);
    if (previous != on_unhandled) {
        MC_LOG_INFO("Crash reporting was displaced by {} and has been taken back", after);
    }
}

void test_crash_if_asked(const char* moment) {
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, "MC_TEST_CRASH") != 0 || !value) return;
    const bool now = strcmp(value, moment) == 0;
    std::free(value);
    if (!now) return;
    MC_LOG_INFO("Deliberately faulting to test the crash report ({})", moment);
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

const char* current_apartment() {
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    if (FAILED(CoGetApartmentType(&type, &qualifier))) {
        return "none -- COM is not initialised on this thread";
    }
    switch (type) {
        case APTTYPE_STA:     return "single-threaded (STA)";
        case APTTYPE_MTA:     return "multithreaded (MTA)";
        case APTTYPE_NA:      return "neutral";
        case APTTYPE_MAINSTA: return "main single-threaded";
        default:              return "unknown";
    }
}

void log_process_context() {
    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }

    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);

    // A window station without a visible desktop is the one environment where
    // a webview cannot be created no matter how healthy everything else is.
    const bool interactive = GetProcessWindowStation() != nullptr &&
                             GetThreadDesktop(GetCurrentThreadId()) != nullptr;

    MC_LOG_INFO("Process: elevated={} session={} interactive_desktop={}",
                elevated, session, interactive);
}

} // namespace midi_composer::shell

#else

namespace midi_composer::shell {
void install_crash_reporting() {}
void reassert_crash_reporting(const char*) {}
void test_crash_if_asked(const char*) {}
void log_process_context() {}
const char* current_apartment() { return "not applicable on this platform"; }
} // namespace midi_composer::shell

#endif

#include "com_apartment.hpp"

#include "base/logger.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

namespace midi_composer::shell {

bool initialize_ui_apartment() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // S_FALSE means it was already a single-threaded apartment, which is the
    // answer we wanted; only the reference count moved.
    if (hr == S_OK || hr == S_FALSE) {
        MC_LOG_INFO("Main thread apartment: {}", current_apartment());
        return true;
    }

    if (hr == RPC_E_CHANGED_MODE) {
        MC_LOG_CRITICAL(
            "The main thread is already in a multithreaded apartment, claimed "
            "before this application ran. A webview cannot be created there.");
        return false;
    }

    MC_LOG_CRITICAL("Could not initialise COM on the main thread: 0x{:08X}",
                    static_cast<unsigned>(hr));
    return false;
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

} // namespace midi_composer::shell

#else

namespace midi_composer::shell {
bool initialize_ui_apartment() { return true; }
const char* current_apartment() { return "not applicable on this platform"; }
} // namespace midi_composer::shell

#endif

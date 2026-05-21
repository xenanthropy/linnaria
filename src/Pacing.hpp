#pragma once
#include <atomic>
#include <cstdint>

// Global pacing rates (Hz; 0 = uncapped). Main loop reads these every
// iteration; HLE modules (e.g., the Octarine log-trigger in HLE_Android)
// write them when boot reaches a known state. Atomic because HLE worker
// threads may also touch them.
namespace Pacing {
    inline std::atomic<uint32_t> game_tick_hz{0};
    inline std::atomic<uint32_t> display_hz{60};
}

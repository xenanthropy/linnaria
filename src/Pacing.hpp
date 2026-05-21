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

    // Set true by GL draw/clear HLE handlers when the game issues something
    // that modifies the back buffer; cleared by the main loop on swap.
    // Prevents presenting a stale back buffer when the game's tick was
    // logic-only (no GL work), which otherwise produces a ping-pong flicker
    // between the latest drawn frame and whatever stale content sits in the
    // other half of the double-buffer.
    inline std::atomic<bool> frame_dirty{false};

    // Gate for SDL input -> native dispatch. Flipped true by the HLE_Android
    // log trigger when "TerrariaInitializer::Run() DONE" fires, which is
    // when the main menu has finished initializing and the game is ready
    // to receive touch/key events. Before that, the splash sequence is
    // still wiring up handler state and dispatching to it crashes.
    inline std::atomic<bool> input_enabled{false};
}

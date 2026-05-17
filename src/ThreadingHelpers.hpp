#pragma once
#include <cstdint>

struct OnceJumpState {
    uint32_t regs[16];
    uint32_t cpsr;
    bool valid = false;
};
// Declared thread_local so each thread has its own saved context.
extern thread_local OnceJumpState once_saved_state;

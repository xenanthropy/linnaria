#pragma once
#include <stdint.h>

struct Watchpoint {
    inline static uint32_t addr = 0;
    inline static bool enabled = false;
};

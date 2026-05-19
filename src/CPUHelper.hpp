#include <stdint.h>

namespace Dynarmic::A32 { class Jit; }

// thread_local ensures each background thread has its own separate pointer
extern thread_local Dynarmic::A32::Jit* active_cpu;
extern thread_local uint32_t active_thread_id;

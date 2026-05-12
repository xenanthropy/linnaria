#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"

namespace HLE::OS {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        ROUTE_REGISTER(router, "abort", [](Dynarmic::A32::Jit* cpu) {
            std::cerr << "\\n[Bionic] Game called abort()!" << std::endl;
            // Throwing a C++ exception will cleanly break out of your cpu.Step() loop
            // and trigger the crash dump in main.cpp so you can see the final registers.
            throw std::runtime_error("Guest intentionally aborted execution.");
        });

        //TODO: exit, raise, __stack_chk_fail, getpid, syscall, ioctl, umask, __cxa_finalize
        // Unneeded: dlopen, dlclose, dlsym, dlerror

        ROUTE_REGISTER(router ,"prctl", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // success
        });

        ROUTE_REGISTER(router, "__cxa_atexit", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // Success
        });

        static uint32_t guest_errno_ptr = 0;
        ROUTE_REGISTER(router, "__errno", [&memory](Dynarmic::A32::Jit* cpu) {
            if (guest_errno_ptr == 0) {
                guest_errno_ptr = memory.AllocateHeap(4);
                memory.Write32(guest_errno_ptr, 0);
            }
            cpu->Regs()[0] = guest_errno_ptr;
        });

        ROUTE_REGISTER(router, "setjmp", [](Dynarmic::A32::Jit* cpu) {
            // Returning 0 tells libpng "We are initializing, no errors yet"
            cpu->Regs()[0] = 0; 
        });

        ROUTE_REGISTER(router, "longjmp", [](Dynarmic::A32::Jit* cpu) {
            // If libpng hits a fatal error, it will call longjmp to bail out.
            throw std::runtime_error("Guest called longjmp! libpng encountered a fatal error.");
        });

    }

}

#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include <mutex>

namespace HLE::OS {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory, ElfLoader& loader) {

        ROUTE_REGISTER(router, "abort", [](Dynarmic::A32::Jit* cpu) {
            std::cerr << "\\n[Bionic] Game called abort()!" << std::endl;
            throw std::runtime_error("Guest intentionally aborted execution.");
        });

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

        // --- C++ Static Guard support (handles local static variable init) ---
        ROUTE_REGISTER(router, "__cxa_guard_acquire", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t guard_ptr = cpu->Regs()[0];
            if (!guard_ptr) {
                cpu->Regs()[0] = 0;
                return;
            }
            // guard variable layout (32-bit): 0 = unlocked, 1 = locked, 2 = initialized
            uint32_t* host = reinterpret_cast<uint32_t*>(memory.GetHostPointer(guard_ptr));
            if (*host == 0) {
                *host = 1;          // lock
                cpu->Regs()[0] = 1; // success, proceed with initialization
            } else {
                cpu->Regs()[0] = 0; // already locked, caller should spin/retry
            }
        });

        ROUTE_REGISTER(router, "__cxa_guard_release", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t guard_ptr = cpu->Regs()[0];
            if (guard_ptr)
                *reinterpret_cast<uint32_t*>(memory.GetHostPointer(guard_ptr)) = 2; // mark initialized
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "__cxa_guard_abort", [&memory](Dynarmic::A32::Jit* cpu) {
            // Called if an exception occurs during initialization; reset guard to 0
            uint32_t guard_ptr = cpu->Regs()[0];
            if (guard_ptr)
                *reinterpret_cast<uint32_t*>(memory.GetHostPointer(guard_ptr)) = 0;
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "dlopen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            int flags = cpu->Regs()[1];

            const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));
            std::cout << "[dlopen] " << path << " (flags=0x" << std::hex << flags << std::dec << ")" << std::endl;

            // Return a fake handle for any system library
            cpu->Regs()[0] = 0xDEAD0001; // non-zero = success
        });

        ROUTE_REGISTER(router, "dlerror", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // NULL, no error
        });

        ROUTE_REGISTER(router, "dlsym", [&memory, &loader](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];
            uint32_t sym_ptr = cpu->Regs()[1];
            const char* sym = reinterpret_cast<const char*>(memory.GetHostPointer(sym_ptr));
            std::cout << "[dlsym] handle=0x" << std::hex << handle << std::dec
                      << " symbol=" << sym << std::endl;

            if (strcmp(sym, "getauxval") == 0) {
                cpu->Regs()[0] = loader.GetThunk("getauxval");
                return;
            }
            // For other symbols, return 0 for now (or we can add more later)
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "dlclose", [](Dynarmic::A32::Jit* cpu) {
            // Nothing to clean up; return 0 (success)
            cpu->Regs()[0] = 0;
        });


        ROUTE_REGISTER(router, "getauxval", [&memory, &loader](Dynarmic::A32::Jit* cpu) {
            unsigned long type = cpu->Regs()[0];
            unsigned long result = 0;

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[getauxval] type=" << type << std::endl;
            }

            switch (type) {
                case 3:   // AT_PHDR
                    result = 0; // not meaningful for us
                    break;
                case 4:   // AT_PHENT
                    result = 32; // size of program header entry
                    break;
                case 5:   // AT_PHNUM
                    result = 0; // unknown
                    break;
                case 6:   // AT_PAGESZ
                    result = 4096;
                    break;
                case 7:   // AT_BASE
                    result = 0; // interpreter base
                    break;
                case 8:   // AT_FLAGS
                    result = 0;
                    break;
                case 9:   // AT_ENTRY
                    result = loader.GetEntryPoint();
                    break;
                case 11:  // AT_UID
                    result = 0;
                    break;
                case 12:  // AT_EUID
                    result = 0;
                    break;
                case 16:  // AT_HWCAP
                    result = (1 << 10) | (1 << 12) | (1 << 13);  // VFPv3 + NEON + VFPv4
                    break;
                case 26:  // AT_HWCAP2
                    result = 0;
                    break;
                default:
                    result = 0;
                    break;
            }
            cpu->Regs()[0] = static_cast<uint32_t>(result);
            cpu->Regs()[1] = static_cast<uint32_t>(result >> 32); // for 64-bit return? but it's unsigned long on 32-bit, so just R0.
            // Actually, on 32-bit ARM, unsigned long is 32 bits, so only R0 is needed.
        });

        ROUTE_REGISTER(router, "sysconf", [](Dynarmic::A32::Jit* cpu) {
            int name = static_cast<int>(cpu->Regs()[0]);
            long result = -1;

            switch (name) {
                case 84:   // _SC_NPROCESSORS_CONF
                case 83:   // _SC_NPROCESSORS_ONLN
                    result = 4; // Nexus 5 has 4 cores
                    break;
                case 30:   // _SC_PAGESIZE
                    result = 4096;
                    break;
                case 2:    // _SC_CLK_TCK
                    result = 100;
                    break;
                // Add more as needed
                default:
                    std::cout << "[sysconf] unknown name=" << name << std::endl;
                    break;
            }

            cpu->Regs()[0] = static_cast<uint32_t>(result);
        });
    }

}

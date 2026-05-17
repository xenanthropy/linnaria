#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <sys/time.h>
#include <chrono>

namespace HLE::Time {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        ROUTE_REGISTER(router, "gettimeofday", [&memory](Dynarmic::A32::Jit* cpu) {
            struct timeval tv;
            ::gettimeofday(&tv, nullptr);
            uint32_t tv_ptr = cpu->Regs()[0];
            if (tv_ptr) {
                memory.Write32(tv_ptr,     static_cast<uint32_t>(tv.tv_sec));
                memory.Write32(tv_ptr + 4, static_cast<uint32_t>(tv.tv_usec));
            }
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "clock_gettime", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t tp_ptr = cpu->Regs()[1];
            
            if (tp_ptr) {
                // Grab the real time from host PC
                auto now = std::chrono::system_clock::now().time_since_epoch();
                uint32_t sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
                uint32_t nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
                
                // Write it into guest's timespec struct
                memory.Write32(tp_ptr, sec);
                memory.Write32(tp_ptr + 4, nsec);
            }
            cpu->Regs()[0] = 0; // Success
        });

        ROUTE_REGISTER(router, "time", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t arg_ptr = cpu->Regs()[0];
            time_t current_time = std::time(nullptr);
            
            // If the game passes a valid pointer, we also have to write the time into that memory address
            if (arg_ptr != 0) {
                memory.Write32(arg_ptr, static_cast<uint32_t>(current_time));
            }
            
            // It always returns the time in R0 as well
            cpu->Regs()[0] = static_cast<uint32_t>(current_time);
        });

        ROUTE_REGISTER(router, "nanosleep", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0;
            // Force Dynarmic to break out of its execution loop
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2); 
        });

        ROUTE_REGISTER(router, "gmtime_r", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t time_ptr   = cpu->Regs()[0];  // guest pointer to time_t
            uint32_t result_ptr = cpu->Regs()[1];  // guest pointer to struct tm

            if (time_ptr == 0 || result_ptr == 0) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Read the 32‑bit time_t value from guest memory
            time_t raw_time = static_cast<time_t>(memory.Read32(time_ptr));

            // Get a host pointer to the struct tm in guest memory
            struct tm* host_result = reinterpret_cast<struct tm*>(memory.GetHostPointer(result_ptr));

            // Call the host gmtime_r – fills the struct in guest memory
            gmtime_r(&raw_time, host_result);

            // Return the guest pointer to the result (success)
            cpu->Regs()[0] = result_ptr;
        });

    }
}

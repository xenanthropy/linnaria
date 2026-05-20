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

            time_t raw_time = static_cast<time_t>(memory.Read32(time_ptr));

            // DO NOT pass the guest struct tm directly to host gmtime_r:
            // host glibc tm = 56 bytes (long/ptr = 8B), Bionic guest tm = 44 bytes
            // (long/ptr = 4B). Writing host-sized struct would overflow by 12B and
            // mis-place tm_gmtoff (guest 36 vs host 40) and tm_zone (guest 40 vs host 48).
            struct tm host_tm{};
            if (gmtime_r(&raw_time, &host_tm) == nullptr) {
                cpu->Regs()[0] = 0;
                return;
            }

            memory.Write32(result_ptr + 0,  static_cast<uint32_t>(host_tm.tm_sec));
            memory.Write32(result_ptr + 4,  static_cast<uint32_t>(host_tm.tm_min));
            memory.Write32(result_ptr + 8,  static_cast<uint32_t>(host_tm.tm_hour));
            memory.Write32(result_ptr + 12, static_cast<uint32_t>(host_tm.tm_mday));
            memory.Write32(result_ptr + 16, static_cast<uint32_t>(host_tm.tm_mon));
            memory.Write32(result_ptr + 20, static_cast<uint32_t>(host_tm.tm_year));
            memory.Write32(result_ptr + 24, static_cast<uint32_t>(host_tm.tm_wday));
            memory.Write32(result_ptr + 28, static_cast<uint32_t>(host_tm.tm_yday));
            memory.Write32(result_ptr + 32, static_cast<uint32_t>(host_tm.tm_isdst));
            memory.Write32(result_ptr + 36, 0); // tm_gmtoff (UTC)
            memory.Write32(result_ptr + 40, 0); // tm_zone (null)

            cpu->Regs()[0] = result_ptr;
        });

    }
}

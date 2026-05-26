#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <sys/time.h>
#include <chrono>
#include <ctime>
#include <cstring>
#include <vector>

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

        ROUTE_REGISTER(router, "nanosleep", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t req_ptr = cpu->Regs()[0];
            if (req_ptr) {
                int32_t tv_sec  = static_cast<int32_t>(memory.Read32(req_ptr));
                int32_t tv_nsec = static_cast<int32_t>(memory.Read32(req_ptr + 4));
        
                if (tv_sec == 0 && tv_nsec == 0) {
                    std::this_thread::yield(); // It's just a 0-wait yield
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(tv_sec) + std::chrono::nanoseconds(tv_nsec));
                }
            } else {
                std::this_thread::yield();
            }
            cpu->Regs()[0] = 0; // Return success
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

        ROUTE_REGISTER(router, "strftime", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t out_ptr  = cpu->Regs()[0];
            uint32_t max      = cpu->Regs()[1];
            uint32_t fmt_ptr  = cpu->Regs()[2];
            uint32_t tm_ptr   = cpu->Regs()[3];

            if (out_ptr == 0 || max == 0 || fmt_ptr == 0 || tm_ptr == 0) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Guest struct tm is the Bionic 44-byte layout (see gmtime_r above).
            struct tm host_tm{};
            host_tm.tm_sec   = static_cast<int>(memory.Read32(tm_ptr + 0));
            host_tm.tm_min   = static_cast<int>(memory.Read32(tm_ptr + 4));
            host_tm.tm_hour  = static_cast<int>(memory.Read32(tm_ptr + 8));
            host_tm.tm_mday  = static_cast<int>(memory.Read32(tm_ptr + 12));
            host_tm.tm_mon   = static_cast<int>(memory.Read32(tm_ptr + 16));
            host_tm.tm_year  = static_cast<int>(memory.Read32(tm_ptr + 20));
            host_tm.tm_wday  = static_cast<int>(memory.Read32(tm_ptr + 24));
            host_tm.tm_yday  = static_cast<int>(memory.Read32(tm_ptr + 28));
            host_tm.tm_isdst = static_cast<int>(memory.Read32(tm_ptr + 32));

            const char* fmt = reinterpret_cast<const char*>(memory.GetHostPointer(fmt_ptr));

            std::vector<char> buf(max);
            size_t n = std::strftime(buf.data(), max, fmt, &host_tm);

            if (n > 0) {
                std::memcpy(memory.GetHostPointer(out_ptr), buf.data(), n + 1); // include NUL
            } else if (max > 0) {
                memory.Write8(out_ptr, 0); // strftime ran out of room: leave buffer terminated
            }
            cpu->Regs()[0] = static_cast<uint32_t>(n);
        });

    }
}

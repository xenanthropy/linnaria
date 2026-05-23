#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <cstring>

#include "Watchpoint.hpp"

namespace HLE::Memory {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {
        
        ROUTE_REGISTER(router, "malloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t size = cpu->Regs()[0];
            cpu->Regs()[0] = memory.AllocateHeap(size, cpu->Regs()[14]);
        });

        ROUTE_REGISTER(router, "calloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nmemb = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t total = nmemb * size;
            if (total == 0) total = 1;
            uint32_t ptr = memory.AllocateHeap(total, cpu->Regs()[14]);
            std::memset(memory.GetHostPointer(ptr), 0, total);
            cpu->Regs()[0] = ptr;
        });

        ROUTE_REGISTER(router, "realloc", [&memory](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = memory.ReallocHeap(cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[14]);
        });


        ROUTE_REGISTER(router, "free", [&memory](Dynarmic::A32::Jit* cpu) {
            memory.FreeHeap(cpu->Regs()[0]);
        });

        ROUTE_REGISTER(router, "memcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src  = cpu->Regs()[1];
            uint32_t n    = cpu->Regs()[2];

            if (n > 0) {
                memory.CheckBoundedWrite(dest, n, "memcpy", cpu->Regs()[14]);
                std::memcpy(memory.GetHostPointer(dest), memory.GetHostPointer(src), n);
            }

            cpu->Regs()[0] = dest;
        });

        ROUTE_REGISTER(router, "memset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint8_t val = static_cast<uint8_t>(cpu->Regs()[1]);
            uint32_t count = cpu->Regs()[2];

            if (count > 0) memory.CheckBoundedWrite(dest, count, "memset", cpu->Regs()[14]);

            for (uint32_t i = 0; i < count; i++) {
                memory.Write8(dest + i, val);
            }
            cpu->Regs()[0] = dest; // memset returns the original pointer
        });

        ROUTE_REGISTER(router, "memmove", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            if (count > 0) memory.CheckBoundedWrite(dest, count, "memmove", cpu->Regs()[14]);

            std::memmove(memory.GetHostPointer(dest), memory.GetHostPointer(src), count);

            cpu->Regs()[0] = dest; // memmove returns the destination pointer
        });

        ROUTE_REGISTER(router, "memcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr1 = cpu->Regs()[0];
            uint32_t ptr2 = cpu->Regs()[1];
            uint32_t num  = cpu->Regs()[2];

            /* IGNORE: debug printing
            // Safely extract up to 16 bytes as printable ASCII
            std::string s1;
            std::string s2;

            for(uint32_t i = 0; i < std::min(num, 16u); i++) {
                char c1 = memory.Read8(ptr1 + i);
                char c2 = memory.Read8(ptr2 + i);
                s1 += (c1 >= 32 && c1 <= 126) ? c1 : '.';
                s2 += (c2 >= 32 && c2 <= 126) ? c2 : '.';
            }

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                //std::cout << "[Router] memcmp (" << num << " bytes): '" << s1 << "' vs '" << s2 << "'" << std::endl;
                std::cout << "[Router] memcmp (" << num << " bytes): '" << s1 << "' (at " << std::hex << ptr1 << std::dec
                          << ") vs '" << s2 << "' (at " << std::hex << ptr2 << std::dec << ")" << std::endl;
            }
            */

            int result = std::memcmp(memory.GetHostPointer(ptr1), memory.GetHostPointer(ptr2), num);
            cpu->Regs()[0] = result;
        });

        ROUTE_REGISTER(router, "memchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
        
            // Search using host memory
            uint8_t* host_ptr = memory.GetHostPointer(ptr);
            void* res = std::memchr(host_ptr, ch, count);
    
            if (res) {
                uint32_t offset = static_cast<uint8_t*>(res) - host_ptr;
                uint32_t guest_addr = ptr + offset;
                cpu->Regs()[0] = guest_addr;
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "wmemcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];
            
            std::wmemcpy(
                reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(src)),
                n
            );
            cpu->Regs()[0] = dest;
        });

        ROUTE_REGISTER(router, "wmemmove", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];

            std::wmemmove(
                reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(src)),
                n
            );
            cpu->Regs()[0] = dest;
        });

        ROUTE_REGISTER(router, "wmemset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            wchar_t ch = static_cast<wchar_t>(cpu->Regs()[1]);
            uint32_t n = cpu->Regs()[2];
            
            std::wmemset(reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)), ch, n);
            cpu->Regs()[0] = dest;
        });

        ROUTE_REGISTER(router, "wmemcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr1 = cpu->Regs()[0];
            uint32_t ptr2 = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];
            
            cpu->Regs()[0] = std::wmemcmp(
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(ptr1)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(ptr2)),
                n
            );
        });

        ROUTE_REGISTER(router, "wmemchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            wchar_t ch = static_cast<wchar_t>(cpu->Regs()[1]);
            uint32_t n = cpu->Regs()[2];
            
            wchar_t* host_ptr = reinterpret_cast<wchar_t*>(memory.GetHostPointer(ptr));
            wchar_t* res = std::wmemchr(host_ptr, ch, n);
            
            if (res) {
                // must translate the host memory address BACK into a 32-bit guest address
                uint32_t byte_offset = reinterpret_cast<uint8_t*>(res) - reinterpret_cast<uint8_t*>(host_ptr);
                cpu->Regs()[0] = ptr + byte_offset;
            } else {
                cpu->Regs()[0] = 0; // NULL
            }
        });

        ROUTE_REGISTER(router, "memmem", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t haystack_ptr = cpu->Regs()[0];
            uint32_t haystack_len = cpu->Regs()[1];
            uint32_t needle_ptr   = cpu->Regs()[2];
            uint32_t needle_len   = cpu->Regs()[3];

            if (needle_len == 0) {
                cpu->Regs()[0] = haystack_ptr;
                return;
            }

            uint8_t* hay = memory.GetHostPointer(haystack_ptr);
            uint8_t* ndl = memory.GetHostPointer(needle_ptr);

            for (uint32_t i = 0; i + needle_len <= haystack_len; i++) {
                if (memcmp(hay + i, ndl, needle_len) == 0) {
                    cpu->Regs()[0] = haystack_ptr + i;
                    return;
                }
            }
            cpu->Regs()[0] = 0; // NULL
        });

    }
}

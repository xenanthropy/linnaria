#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <cstring>

namespace HLE::Memory {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {
        
        ROUTE_REGISTER(router, "malloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t size = cpu->Regs()[0];
            cpu->Regs()[0] = memory.AllocateHeap(size); 
        });

        ROUTE_REGISTER(router, "calloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nmemb = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t total = nmemb * size;
            if (total == 0) total = 1;
            uint32_t ptr = memory.AllocateHeap(total);
            std::memset(memory.GetHostPointer(ptr), 0, total);
            cpu->Regs()[0] = ptr;
        });

        ROUTE_REGISTER(router, "realloc", [&memory](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = memory.ReallocHeap(cpu->Regs()[0], cpu->Regs()[1]);
        });


        ROUTE_REGISTER(router, "free", [&memory](Dynarmic::A32::Jit* cpu) {
            memory.FreeHeap(cpu->Regs()[0]);
        });


        ROUTE_REGISTER(router, "memcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            /*
            std::cout << "-------^ memcpy ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;
            */
            
            std::memcpy(memory.GetHostPointer(dest), memory.GetHostPointer(src), count);
            cpu->Regs()[0] = dest;
        });

        ROUTE_REGISTER(router, "memset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint8_t val = static_cast<uint8_t>(cpu->Regs()[1]);
            uint32_t count = cpu->Regs()[2];

            /*
            std::cout << "-------^ memset ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;
            */
            for (uint32_t i = 0; i < count; i++) {
                memory.Write8(dest + i, val);
            }
            cpu->Regs()[0] = dest; // memset returns the original pointer
        });

        ROUTE_REGISTER(router, "memmove", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            
            std::memmove(memory.GetHostPointer(dest), memory.GetHostPointer(src), count);
            
            cpu->Regs()[0] = dest; // memmove returns the destination pointer
        });

        ROUTE_REGISTER(router, "memcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr1 = cpu->Regs()[0];
            uint32_t ptr2 = cpu->Regs()[1];
            uint32_t num = cpu->Regs()[2];
            
            // Safely extract up to 16 bytes as printable ASCII
            std::string s1;
            std::string s2;

            for(uint32_t i = 0; i < std::min(num, 16u); i++) {
                char c1 = memory.Read8(ptr1 + i);
                char c2 = memory.Read8(ptr2 + i);
                s1 += (c1 >= 32 && c1 <= 126) ? c1 : '.';
                s2 += (c2 >= 32 && c2 <= 126) ? c2 : '.';
            }
            // std::cout << "[Router] memcmp (" << num << " bytes): '" << s1 << "' vs '" << s2 << "'" << std::endl;
            
            int result = std::memcmp(memory.GetHostPointer(ptr1), memory.GetHostPointer(ptr2), num);

            // --- THE BULLETPROOF EXTENSION BYPASS ---
            if (num == 4) {
                std::string s2(reinterpret_cast<const char*>(memory.GetHostPointer(ptr2)), 4);
                
                // If the engine asks "Is this a .png?", we scream "YES!"
                if (s2 == ".png") {
                    std::cout << "\n[Hack] Engine asked for .png. Forcing match to bypass locale corruption!" << std::endl;
                    result = 0; // 0 = Strings match perfectly
                }
            }

            cpu->Regs()[0] = result;
        });

        ROUTE_REGISTER(router, "memchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
    
            bool is_xml = (0x55b9dbc0 != 0 && ptr >= 0x55b9dbc0 && ptr < (0x55b9dbc0 + 0x223b));
    
            // Search using host memory
            uint8_t* host_ptr = memory.GetHostPointer(ptr);
            void* res = std::memchr(host_ptr, ch, count);
    
            if (res) {
                uint32_t offset = static_cast<uint8_t*>(res) - host_ptr;
                uint32_t guest_addr = ptr + offset;
                if (is_xml) {
                    std::cout << "[memchr] XML: ptr=0x" << std::hex << ptr
                              << " ch=0x" << ch << " ('" << (char)ch << "')"
                              << " count=" << std::dec << count
                              << " found at offset " << offset
                              << " -> 0x" << std::hex << guest_addr << std::dec
                              << std::endl;
                }
                cpu->Regs()[0] = guest_addr;
            } else {
                if (is_xml) {
                    std::cout << "[memchr] XML: ptr=0x" << std::hex << ptr
                              << " ch=0x" << ch << " ('" << (char)ch << "')"
                              << " count=" << std::dec << count
                              << " NOT FOUND" << std::endl;
                }
                cpu->Regs()[0] = 0;
            }
        });

        //TODO: memmem
        //ROUTE_REGISTER(router, "memmem", [&memory](Dynarmic::A32::Jit* cpu) { });

        ROUTE_REGISTER(router, "wmemcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];
            
            std::wmemcpy(
                reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(src)),
                n
            );
            cpu->Regs()[0] = dest; // Returns destination pointer
        });

        ROUTE_REGISTER(router, "wmemset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            wchar_t ch = static_cast<wchar_t>(cpu->Regs()[1]);
            uint32_t n = cpu->Regs()[2];
            
            std::wmemset(reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)), ch, n);
            cpu->Regs()[0] = dest; // Returns destination pointer
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
                // We must translate the host memory address BACK into a 32-bit guest address!
                uint32_t byte_offset = reinterpret_cast<uint8_t*>(res) - reinterpret_cast<uint8_t*>(host_ptr);
                cpu->Regs()[0] = ptr + byte_offset;
            } else {
                cpu->Regs()[0] = 0; // NULL
            }
        });

        //TODO: wmemmove
        //ROUTE_REGISTER(router, "wmemmove", [&memory](Dynarmic::A32::Jit* cpu) { });

    }

}

#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <sys/mman.h>

class GuestMemory {
public:
    static constexpr uint32_t CODE_BASE = 0x40000000;
    static constexpr uint32_t MEMORY_SIZE = 512 * 1024 * 1024; // 512MB

    uint8_t* fastmem_base = nullptr;

    uint32_t current_heap_ptr = CODE_BASE + 0x10000000;
    
    // RESTORED: Dynarmic needs this for safe memory fallbacks!
    std::array<std::uint8_t*, 1048576> page_table{};

    GuestMemory() {
        // Reserve 4GB virtual address space
        fastmem_base = (uint8_t*)mmap(nullptr, 0x100000000ULL, PROT_NONE, 
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fastmem_base == MAP_FAILED) throw std::runtime_error("Failed to reserve 4GB fastmem space!");

        // Commit the 512MB we need at the exact guest offset
        void* code_ptr = mmap(fastmem_base + CODE_BASE, MEMORY_SIZE, 
                              PROT_READ | PROT_WRITE, 
                              MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code_ptr == MAP_FAILED) throw std::runtime_error("Failed to commit guest memory!");

        // 3. Commit the 4KB Kernel Helper Page at 0xFFFF0000
        void* kuser_ptr = mmap(fastmem_base + 0xFFFF0000, 4096, 
                               PROT_READ | PROT_WRITE, 
                               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (kuser_ptr == MAP_FAILED) throw std::runtime_error("Failed to commit kernel helper page!");

        /////// new (was having issues with it forceably reading EF000000, no function call)
        // 4. Commit the C-Type Hack Page at 0xEF000000 (64KB size)
        void* ctype_ptr = mmap(fastmem_base + 0xEF000000, 0x10000, 
                               PROT_READ | PROT_WRITE, 
                               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ctype_ptr == MAP_FAILED) throw std::runtime_error("Failed to commit ctype hack page!");

        // Map 16 pages (64KB / 4KB)
        for (int i = 0; i < 16; i++) {
            page_table[0xEF000 + i] = (uint8_t*)ctype_ptr + (i * 4096);
        }
        //////// endnew

        // Initialize the page table to point to our Fastmem base
        page_table.fill(nullptr);
        for (uint32_t i = 0; i < MEMORY_SIZE / 4096; i++) {
            page_table[(CODE_BASE / 4096) + i] = fastmem_base + CODE_BASE + (i * 4096);
        }

        // Add the Kernel Helper Page to the Dynarmic page table
        page_table[0xFFFF0] = (uint8_t*)kuser_ptr;

        // Inject ARM machine code for __kuser_cmpxchg at 0xFFFF0FC0
        // Signature: int cmpxchg(int oldval (R0), int newval (R1), int* ptr (R2))
        uint32_t* cmpxchg = reinterpret_cast<uint32_t*>((uint8_t*)kuser_ptr + 0x0FC0);
        cmpxchg[0] = 0xe5923000; // ldr r3, [r2]      (Load current value)
        cmpxchg[1] = 0xe1530000; // cmp r3, r0        (Compare with expected oldval)
        cmpxchg[2] = 0x1a000002; // bne 2f            (If not equal, skip to fail)
        cmpxchg[3] = 0xe5821000; // str r1, [r2]      (Store newval)
        cmpxchg[4] = 0xe3a00000; // mov r0, #0        (Success: return 0)
        cmpxchg[5] = 0xe12fff1e; // bx lr             (Return)
        cmpxchg[6] = 0xe3a00001; // mov r0, #1        (Fail: return 1)
        cmpxchg[7] = 0xe12fff1e; // bx lr             (Return)

        // Inject __kuser_memory_barrier at 0xFFFF0FA0
        uint32_t* dmb = reinterpret_cast<uint32_t*>((uint8_t*)kuser_ptr + 0x0FA0);
        dmb[0] = 0xf57ff05f;     // dmb sy            (Data Memory Barrier)
        dmb[1] = 0xe12fff1e;     // bx lr             (Return)
    }

    ~GuestMemory() {
        if (fastmem_base != MAP_FAILED) munmap(fastmem_base, 0x100000000ULL);
    }

    uint8_t* GetHostPointer(uint32_t vaddr) {
        // 1. Allow access to the 4KB Kernel Helper Page
        if (vaddr >= 0xFFFF0000 && vaddr < 0xFFFF1000) {
            return fastmem_base + vaddr;
        }

        // 2. Allow access to the C-Type Hack Page
        if (vaddr >= 0xEF000000 && vaddr < 0xEF010000) return fastmem_base + vaddr;
        
        if (vaddr < CODE_BASE || vaddr >= CODE_BASE + MEMORY_SIZE) {
            std::cerr << "[Memory] WARNING: Out of bounds access at 0x" << std::hex << vaddr << std::dec << std::endl;
            dummy_memory = 0;
            return reinterpret_cast<uint8_t*>(&dummy_memory);
        }
        return fastmem_base + vaddr;
    }

    uint8_t Read8(uint32_t vaddr) { return *GetHostPointer(vaddr); }
    void Write8(uint32_t vaddr, uint8_t val) { *GetHostPointer(vaddr) = val; }
    void Write32(uint32_t vaddr, uint32_t val) {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(GetHostPointer(vaddr));
        *ptr = val;
    }
    uint32_t Read32(uint32_t vaddr) {
        uint32_t* ptr = reinterpret_cast<uint32_t*>(GetHostPointer(vaddr));
        return *ptr;
    }

    uint32_t AllocateHeap(uint32_t size) {
        // Align to 8 bytes to prevent ARM alignment faults
        if (size % 8 != 0) {
            size += 8 - (size % 8);
        }
        
        uint32_t ptr = current_heap_ptr;
        current_heap_ptr += size;
        
        if (current_heap_ptr >= CODE_BASE + MEMORY_SIZE) {
            throw std::runtime_error("Guest Heap Out of Memory!");
        }
        
        return ptr;
    }

    uint32_t AllocateCtypeArray() {
        // Allocate 257 bytes: indices -1 … 255
        uint32_t ctype_addr = AllocateHeap(257);
        uint8_t* ctype = GetHostPointer(ctype_addr);
        // Fill standard ASCII flags
        for (int i = 0; i < 256; i++) {
            uint8_t flags = 0;
            if (i >= 'A' && i <= 'Z') flags |= 0x01; // _U (upper)
            if (i >= 'a' && i <= 'z') flags |= 0x02; // _L (lower)
            if (i >= '0' && i <= '9') flags |= 0x04; // _N (digit)
            if (i == ' '  || i == '\t' || i == '\r' || i == '\n' ||
                i == '\v' || i == '\f') flags |= 0x08; // _S (space)
            ctype[i + 1] = flags;
        }
        // Return pointer to element 0 (the table is indexed from -1)
        return ctype_addr + 1;
    }

private:
    uint32_t dummy_memory = 0;
};

#pragma once
#include <SyscallRouter.hpp>
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <sys/mman.h>
#include <queue>

#include <cstring>
#include <map>
#include <unordered_map>
#include <mutex>

class GuestMemory {
public:
    static constexpr uint32_t CODE_BASE = 0x40000000;
    //static constexpr uint32_t MEMORY_SIZE = 512 * 1024 * 1024; // 512MB
    static constexpr uint32_t MEMORY_SIZE = 1024 * 1024 * 1024; // 1024MB

    uint8_t* fastmem_base = nullptr;

    // Dynarmic needs this for safe memory fallbacks
    std::array<std::uint8_t*, 1048576> page_table{};

    uint32_t current_heap_ptr = CODE_BASE + 0x10000000;
    std::mutex allocator_mutex;
    
    std::map<uint32_t, uint32_t> free_blocks;           // Address -> Size
    std::unordered_map<uint32_t, uint32_t> allocations; // Address -> Size

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

        // (was having issues with it forceably reading EF000000, no function call)
        // 4. Commit the C-Type Hack Page at 0xEF000000 (64KB size)
        void* ctype_ptr = mmap(fastmem_base + 0xEF000000, 0x10000, 
                               PROT_READ | PROT_WRITE, 
                               MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ctype_ptr == MAP_FAILED) throw std::runtime_error("Failed to commit ctype hack page!");

        // Map 16 pages (64KB / 4KB)
        for (int i = 0; i < 16; i++) {
            page_table[0xEF000 + i] = (uint8_t*)ctype_ptr + (i * 4096);
        }

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
        // Allow access to the 4KB Kernel Helper Page
        if (vaddr >= 0xFFFF0000 && vaddr < 0xFFFF1000) {
            return fastmem_base + vaddr;
        }

        // Allow access to the C-Type Hack Page
        if (vaddr >= 0xEF000000 && vaddr < 0xEF010000) return fastmem_base + vaddr;
        
        if (vaddr < CODE_BASE || vaddr >= CODE_BASE + MEMORY_SIZE) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "[Memory] WARNING: Out of bounds access at 0x" << std::hex << vaddr << std::dec << std::endl;
            //std::cerr << "[Memory] WARNING: Out of bounds access at 0x" << std::hex << vaddr << std::dec << std::endl;
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

    uint64_t Read64(uint32_t vaddr) {
        uint64_t lo = Read32(vaddr);
        uint64_t hi = Read32(vaddr + 4);
        return lo | (hi << 32);
    }

    void Write64(uint32_t vaddr, uint64_t value) {
        Write32(vaddr, static_cast<uint32_t>(value & 0xFFFFFFFF));
        Write32(vaddr + 4, static_cast<uint32_t>(value >> 32));
    }

    uint32_t AllocateHeap(uint32_t size) {
        if (size == 0) size = 8;
        if (size % 8 != 0) size += 8 - (size % 8); // Align to 8 bytes

        std::lock_guard<std::mutex> lock(allocator_mutex);

        // Search for a free block that fits (First-Fit)
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
            if (it->second >= size) {
                uint32_t addr = it->first;
                uint32_t block_size = it->second;
                free_blocks.erase(it);

                // Split block if there's enough leftover space
                if (block_size > size + 8) {
                    free_blocks[addr + size] = block_size - size;
                    allocations[addr] = size;
                } else {
                    allocations[addr] = block_size;
                }
                return addr;
            }
        }

        // Fallback to Bump Allocator if no free blocks are large enough
        uint32_t ptr = current_heap_ptr;
        current_heap_ptr += size;
        if (current_heap_ptr >= CODE_BASE + MEMORY_SIZE) {
            throw std::runtime_error("Guest Heap Out of Memory!");
        }
        allocations[ptr] = size;
        return ptr;
    }

    void FreeHeap(uint32_t ptr) {
        if (ptr == 0) return;

        std::lock_guard<std::mutex> lock(allocator_mutex);
        auto it = allocations.find(ptr);
        if (it != allocations.end()) {
            uint32_t size = it->second;
            allocations.erase(it);

            free_blocks[ptr] = size;

            // Coalesce adjacent free blocks to prevent fragmentation
            auto current = free_blocks.find(ptr);
            
            auto next = std::next(current); // Merge forward
            if (next != free_blocks.end() && current->first + current->second == next->first) {
                current->second += next->second;
                free_blocks.erase(next);
            }

            if (current != free_blocks.begin()) { // Merge backward
                auto prev = std::prev(current);
                if (prev->first + prev->second == current->first) {
                    prev->second += current->second;
                    free_blocks.erase(current);
                }
            }
        }
    }

    uint32_t ReallocHeap(uint32_t ptr, uint32_t new_size) {
        if (ptr == 0) return AllocateHeap(new_size);
        if (new_size == 0) { FreeHeap(ptr); return 0; }
        
        allocator_mutex.lock();
        auto it = allocations.find(ptr);
        if (it == allocations.end()) {
            allocator_mutex.unlock();
            return 0; // Invalid pointer
        }
        uint32_t old_size = it->second;
        allocator_mutex.unlock();

        if (old_size >= new_size) return ptr; // Lazy: keep existing if it's large enough

        // Reallocate, copy data, and free old block
        uint32_t new_ptr = AllocateHeap(new_size);
        std::memcpy(GetHostPointer(new_ptr), GetHostPointer(ptr), old_size);
        FreeHeap(ptr);
        return new_ptr;
    }

    uint32_t AllocateCtypeArray() {
        // Already allocated? Just return the flags address
        if (ctype_flags_addr != 0) return ctype_flags_addr;
    
        // Allocate three separate 257-byte tables
        uint32_t flags_addr  = AllocateHeap(257);
        uint32_t tolower_addr = AllocateHeap(257);
        uint32_t toupper_addr = AllocateHeap(257);
    
        uint8_t* flags  = GetHostPointer(flags_addr);
        uint8_t* tolower = GetHostPointer(tolower_addr);
        uint8_t* toupper = GetHostPointer(toupper_addr);
    
        #define _U 0x01
        #define _L 0x02
        #define _N 0x04
        #define _S 0x08
        #define _P 0x10
        #define _C 0x20
        #define _B 0x40
        #define _X 0x80

        for (int i = 0; i < 256; ++i) {
            uint8_t f = 0;
            if (i >= 'A' && i <= 'Z') f |= _U;
            if (i >= 'a' && i <= 'z') f |= _L;
            if (i >= '0' && i <= '9') f |= _N;
            // whitespace
            if (i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\v' || i == '\f') {
                f |= _S;
                if (i == ' ' || i == '\t') f |= _B;
            }
            // punctuation
            if (i >= 32 && i <= 126 && !(f & (_U|_L|_N|_S))) f |= _P;
            // control
            if (i < 32 || i == 0x7F) f |= _C;
            // hex digit
            if ((i >= '0' && i <= '9') || (i >= 'A' && i <= 'F') || (i >= 'a' && i <= 'f')) f |= _X;
        
            flags[i+1] = f;
        
            // tolower: uppercase -> lowercase, else identity
            tolower[i+1] = (i >= 'A' && i <= 'Z') ? (i + 0x20) : i;
            // toupper: lowercase -> uppercase, else identity
            toupper[i+1] = (i >= 'a' && i <= 'z') ? (i - 0x20) : i;
        }
        flags[0] = tolower[0] = toupper[0] = 0;  // entry for EOF
    
        // Store the addresses (pointing to element 0, i.e. table[0])
        ctype_flags_addr   = flags_addr; // + 1;
        ctype_tolower_addr = tolower_addr; // + 1;
        ctype_toupper_addr = toupper_addr; // + 1;
    
        // Return flags table address (for backward compatibility)
        return ctype_flags_addr;
    }

    uint32_t GetCtypeFlagsAddr() const   { return ctype_flags_addr; }
    uint32_t GetCtypeTolowerAddr() const { return ctype_tolower_addr; }
    uint32_t GetCtypeToupperAddr() const { return ctype_toupper_addr; }

private:
    uint32_t dummy_memory = 0;
    uint32_t ctype_flags_addr   = 0;
    uint32_t ctype_tolower_addr = 0;
    uint32_t ctype_toupper_addr = 0;
};

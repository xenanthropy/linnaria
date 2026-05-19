#pragma once
#include <SyscallRouter.hpp>
#include <CPUHelper.hpp>
#include <cstdlib>
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
#include <optional>
#include <thread>
#include <algorithm>

#include "Watchpoint.hpp"

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

    struct AllocInfo {
        uint32_t size;
        uint32_t caller_pc;   // guest LR at allocation time (0 if unknown)
        std::thread::id tid;  // host thread that performed the allocation
    };

    std::map<uint32_t, uint32_t>   free_blocks;  // Address -> Size
    std::map<uint32_t, AllocInfo>  allocations;  // Address -> AllocInfo (sorted for range lookup)

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
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "\n[CRASH TRAP] OOB in GetHostPointer: 0x" << std::hex << vaddr << std::dec << "!\n";
                if (active_cpu) {
                    for (int i = 0; i < 15; i++) {
                        std::cout << "  R" << i << " = 0x" << std::hex << active_cpu->Regs()[i] << std::dec << "\n";
                    }
                    std::cout << "  PC   = 0x" << std::hex << active_cpu->Regs()[15] << "\n"
                              << "  CPSR = 0x" << active_cpu->Cpsr() << std::dec << "\n";

                    // DEBUG: find faulty caller
                    std::cout << "Faulty Caller: 0x" << std::hex << Read32(0x5208cb64) << std::dec << std::endl;
                } else {
                    std::cout << "  (active_cpu is null, cannot dump registers)\n";
                }

                std::exit(1);
            }
            /*
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "[Memory] WARNING: Out of bounds access at 0x" << std::hex << vaddr << std::dec << std::endl;
            //std::cerr << "[Memory] WARNING: Out of bounds access at 0x" << std::hex << vaddr << std::dec << std::endl;
            dummy_memory = 0;
            return reinterpret_cast<uint8_t*>(&dummy_memory);
            */
        }
        return fastmem_base + vaddr;
    }

    uint8_t Read8(uint32_t vaddr) { return *GetHostPointer(vaddr); }
    void Write8(uint32_t vaddr, uint8_t val) { *GetHostPointer(vaddr) = val; }

    uint16_t Read16(uint32_t vaddr) {
        uint16_t* ptr = reinterpret_cast<uint16_t*>(GetHostPointer(vaddr));
        return *ptr;
    }

    void Write16(uint32_t vaddr, uint16_t val) {
        uint16_t* ptr = reinterpret_cast<uint16_t*>(GetHostPointer(vaddr));
        *ptr = val;
    }

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

    uint32_t AllocateHeap(uint32_t size, uint32_t caller_pc = 0) {
        if (size == 0) size = 8;
        if (size % 8 != 0) size += 8 - (size % 8); // Align to 8 bytes

        uint32_t addr;
        uint32_t alloc_size;
        {
            std::lock_guard<std::mutex> lock(allocator_mutex);

            // Search for a free block that fits (First-Fit)
            bool from_free_list = false;
            for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
                if (it->second >= size) {
                    addr = it->first;
                    uint32_t block_size = it->second;
                    free_blocks.erase(it);

                    // Split block if there's enough leftover space
                    if (block_size > size + 8) {
                        free_blocks[addr + size] = block_size - size;
                        alloc_size = size;
                    } else {
                        alloc_size = block_size;
                    }
                    allocations[addr] = {alloc_size, caller_pc, std::this_thread::get_id()};
                    from_free_list = true;
                    break;
                }
            }

            if (!from_free_list) {
                // Fallback to Bump Allocator if no free blocks are large enough
                addr = current_heap_ptr;
                current_heap_ptr += size;
                if (current_heap_ptr >= CODE_BASE + MEMORY_SIZE) {
                    throw std::runtime_error("Guest Heap Out of Memory!");
                }
                alloc_size = size;
                allocations[addr] = {alloc_size, caller_pc, std::this_thread::get_id()};
            }
        }

        // Diagnostic: if the returned block overlaps any registered stack range,
        // we've handed out memory that aliases a live thread's stack. Catch it
        // at the moment of corruption rather than at the eventual POP.
        auto hit = Watchpoint::Find(addr, alloc_size);
        if (hit) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\n[AllocateHeap FATAL] Returned block [0x" << std::hex << addr
                      << ", 0x" << (addr + alloc_size) << ") overlaps live watchpoint range ["
                      << "0x" << hit->lo << ", 0x" << hit->hi << ") tag='"
                      << (hit->tag ? hit->tag : "?") << "' owner=" << Watchpoint::TidString(hit->owner)
                      << " caller_pc=0x" << caller_pc << std::dec
                      << " current_tid=" << Watchpoint::TidString(std::this_thread::get_id()) << "\n";
            DumpAllocationsNear(addr);
            std::exit(1);
        }

        return addr;
    }

    void FreeHeap(uint32_t ptr) {
        if (ptr == 0) return;

        std::lock_guard<std::mutex> lock(allocator_mutex);
        auto it = allocations.find(ptr);
        if (it != allocations.end()) {
            uint32_t size = it->second.size;
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

    uint32_t ReallocHeap(uint32_t ptr, uint32_t new_size, uint32_t caller_pc = 0) {
        if (ptr == 0) return AllocateHeap(new_size, caller_pc);
        if (new_size == 0) { FreeHeap(ptr); return 0; }

        allocator_mutex.lock();
        auto it = allocations.find(ptr);
        if (it == allocations.end()) {
            allocator_mutex.unlock();
            return 0; // Invalid pointer
        }
        uint32_t old_size = it->second.size;
        allocator_mutex.unlock();

        if (old_size >= new_size) return ptr; // Lazy: keep existing if it's large enough

        // Reallocate, copy data, and free old block
        uint32_t new_ptr = AllocateHeap(new_size, caller_pc);
        std::memcpy(GetHostPointer(new_ptr), GetHostPointer(ptr), old_size);
        FreeHeap(ptr);
        return new_ptr;
    }

    // Returns the AllocInfo for the live allocation containing `ptr`, or
    // std::nullopt if `ptr` is not inside any tracked heap block.
    struct AllocLookup {
        uint32_t base;
        uint32_t size;
        uint32_t caller_pc;
        std::thread::id tid;
    };
    std::optional<AllocLookup> FindAllocation(uint32_t ptr) {
        std::lock_guard<std::mutex> lock(allocator_mutex);
        auto it = allocations.upper_bound(ptr);
        if (it == allocations.begin()) return std::nullopt;
        --it;
        if (ptr >= it->first && ptr < it->first + it->second.size) {
            return AllocLookup{it->first, it->second.size, it->second.caller_pc, it->second.tid};
        }
        return std::nullopt;
    }

    // Bounds-check a bulk write of `n` bytes to `dest`. Fatal-on-fail per
    // the user's "instrument fatal" preference. Two checks:
    //   1) If `dest` lives inside a tracked allocation, the write must not
    //      run past that allocation's end.
    //   2) The range [dest, dest+n) must not overlap a watched stack range
    //      owned by another host thread.
    // The first writes-into-untracked-memory case (e.g. the main thread's
    // stack at 0x7FFF0000, ELF .data, etc.) is silently allowed.
    void CheckBoundedWrite(uint32_t dest, uint32_t n, const char* op, uint32_t caller_pc) {
        if (n == 0) return;

        auto info = FindAllocation(dest);
        if (info) {
            uint32_t alloc_end = info->base + info->size;
            if (dest + n > alloc_end) {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "\n[CheckBoundedWrite FATAL] " << op
                          << " overruns allocation!\n"
                          << "  dest=0x" << std::hex << dest
                          << " n=" << std::dec << n
                          << " caller_pc=0x" << std::hex << caller_pc << std::dec << "\n"
                          << "  allocation: [0x" << std::hex << info->base << ", 0x"
                          << alloc_end << ") size=" << std::dec << info->size
                          << " alloc_caller_pc=0x" << std::hex << info->caller_pc << std::dec
                          << " alloc_tid=" << Watchpoint::TidString(info->tid)
                          << " current_tid=" << Watchpoint::TidString(std::this_thread::get_id())
                          << "\n  overrun_by=" << (dest + n - alloc_end) << " bytes\n";
                DumpAllocationsNear(dest);
                std::exit(1);
            }
        }

        auto hit = Watchpoint::Find(dest, n);
        if (hit && hit->owner != std::this_thread::get_id()) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\n[CheckBoundedWrite FATAL] " << op
                      << " writes into another thread's watched range!\n"
                      << "  dest=0x" << std::hex << dest
                      << " n=" << std::dec << n
                      << " caller_pc=0x" << std::hex << caller_pc << std::dec << "\n"
                      << "  range: [0x" << std::hex << hit->lo << ", 0x" << hit->hi
                      << ") tag='" << (hit->tag ? hit->tag : "?")
                      << "' owner=" << Watchpoint::TidString(hit->owner)
                      << " current_tid=" << Watchpoint::TidString(std::this_thread::get_id())
                      << std::dec << "\n";
            DumpAllocationsNear(dest);
            std::exit(1);
        }
    }

    // Print the up to N live allocations whose base address is closest to
    // `addr`, plus the registered watchpoint ranges. Called from crash traps.
    void DumpAllocationsNear(uint32_t addr, size_t n = 8) {
        std::vector<std::pair<uint32_t, AllocInfo>> snapshot;
        {
            std::lock_guard<std::mutex> lock(allocator_mutex);
            snapshot.reserve(allocations.size());
            for (auto& kv : allocations) snapshot.emplace_back(kv.first, kv.second);
        }
        std::sort(snapshot.begin(), snapshot.end(),
                  [addr](const auto& a, const auto& b) {
                      auto da = a.first > addr ? a.first - addr : addr - a.first;
                      auto db = b.first > addr ? b.first - addr : addr - b.first;
                      return da < db;
                  });

        std::cout << "[AllocDump] " << snapshot.size() << " live allocations, showing "
                  << std::min(n, snapshot.size()) << " closest to 0x"
                  << std::hex << addr << std::dec << ":\n";
        for (size_t i = 0; i < std::min(n, snapshot.size()); i++) {
            auto& [base, info] = snapshot[i];
            std::cout << "  [0x" << std::hex << base << ", 0x" << (base + info.size)
                      << ") size=" << std::dec << info.size
                      << " caller_pc=0x" << std::hex << info.caller_pc << std::dec
                      << " tid=" << Watchpoint::TidString(info.tid);
            if (addr >= base && addr < base + info.size) {
                std::cout << "   <-- CONTAINS 0x" << std::hex << addr << std::dec;
            }
            std::cout << "\n";
        }

        auto ranges = Watchpoint::Snapshot();
        std::cout << "[AllocDump] " << ranges.size() << " watched ranges:\n";
        for (auto& r : ranges) {
            std::cout << "  [0x" << std::hex << r.lo << ", 0x" << r.hi << ") tag='"
                      << (r.tag ? r.tag : "?") << "' owner="
                      << Watchpoint::TidString(r.owner) << std::dec;
            if (addr >= r.lo && addr < r.hi) {
                std::cout << "   <-- CONTAINS 0x" << std::hex << addr << std::dec;
            }
            std::cout << "\n";
        }
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

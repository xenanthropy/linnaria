#pragma once
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <string>
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "Config.hpp"
#include "CPUHelper.hpp"
#include <mutex>

#include "Watchpoint.hpp"

class EmuCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    EmuCallbacks(GuestMemory& mem, ElfLoader& loader, SyscallRouter& router)
        : mem(mem), loader(loader), router(router) {}

    // --- 8/16/32/64-bit memory ---
    uint8_t  MemoryRead8(uint32_t vaddr)  override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead8)");
        return *mem.GetHostPointer(vaddr);
    }
    void MemoryWrite8(uint32_t vaddr, uint8_t value) override {
        CheckCrossThreadWrite(vaddr, 1, value);
        *mem.GetHostPointer(vaddr) = value;
    }

    uint16_t MemoryRead16(uint32_t vaddr) override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead16)");
        return MemoryRead8(vaddr) | (uint16_t(MemoryRead8(vaddr + 1)) << 8);
    }
    void MemoryWrite16(uint32_t vaddr, uint16_t value) override {
        CheckCrossThreadWrite(vaddr, 2, value);
        MemoryWrite8(vaddr,     value & 0xFF);
        MemoryWrite8(vaddr + 1, (value >> 8) & 0xFF);
    }

    uint32_t MemoryRead32(uint32_t vaddr)  override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead32)");
        return mem.Read32(vaddr);
    }
    void MemoryWrite32(uint32_t vaddr, uint32_t value)  override {
        CheckCrossThreadWrite(vaddr, 4, value);
        if constexpr (Config::Prints::stackWriteTrap) {
            // Catch a stray write into the top of the main stack -- where
            // nativeTouchEvent's saved registers live. The corrupting value
            // turned out to be a touch coordinate as a float (e.g. 268.0f =
            // 0x43860000), so trap on values in the float-coordinate range
            // [32.0, 8192.0] -> [0x42000000, 0x46000000); that excludes code
            // addresses (< ~0x41000000) and heap pointers (>= 0x50000000).
            bool top_of_stack = (vaddr >= 0x7FEFFF00 && vaddr < 0x7FF00000);
            bool coord_like   = (value >= 0x42000000 && value < 0x46000000);
            if (top_of_stack && coord_like) {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[StackTrap] thread " << active_thread_id
                          << " wrote 0x" << std::hex << value
                          << " -> 0x" << vaddr
                          << "  (writer PC=0x" << (cpu ? cpu->Regs()[15] : 0)
                          << " LR=0x" << (cpu ? cpu->Regs()[14] : 0) << ")"
                          << std::dec << "\n";
            }
        }
        mem.Write32(vaddr, value);
    }

    uint64_t MemoryRead64(uint32_t vaddr)  override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead64)");
        return uint64_t(MemoryRead32(vaddr)) | (uint64_t(MemoryRead32(vaddr + 4)) << 32);
    }
    void MemoryWrite64(uint32_t vaddr, uint64_t value) override {
        CheckCrossThreadWrite(vaddr, 8, value);
        MemoryWrite32(vaddr,     uint32_t(value));
        MemoryWrite32(vaddr + 4, uint32_t(value >> 32));
    }

    // If a guest write hits a watched range owned by a different host thread,
    // dump full CPU state + the allocation map and exit. This is the moment
    // we want to catch the heap-vs-stack aliasing bug.
    void CheckCrossThreadWrite(uint32_t vaddr, uint32_t size, uint64_t value) {
        auto hit = Watchpoint::Find(vaddr, size);
        if (!hit) return;
        if (hit->owner == std::this_thread::get_id()) return;

        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n[Watchpoint] CROSS-THREAD WRITE caught!\n"
                  << "  vaddr=0x" << std::hex << vaddr << " size=" << std::dec << size
                  << " value=0x" << std::hex << value << std::dec << "\n"
                  << "  range: [0x" << std::hex << hit->lo << ", 0x" << hit->hi
                  << ") tag='" << (hit->tag ? hit->tag : "?")
                  << "' owner=" << Watchpoint::TidString(hit->owner)
                  << " current_tid=" << Watchpoint::TidString(std::this_thread::get_id())
                  << std::dec << "\n";
        DumpCpuState();
        mem.DumpAllocationsNear(vaddr);
        std::exit(1);
    }

    void DumpCpuState() {
        if (!cpu) return;
        for (int i = 0; i < 13; i++) {
            std::cout << "  R" << i << " = 0x" << std::hex << cpu->Regs()[i] << std::dec << "\n";
        }
        std::cout << "  SP   = 0x" << std::hex << cpu->Regs()[13] << "\n"
                  << "  LR   = 0x" << cpu->Regs()[14] << "\n"
                  << "  PC   = 0x" << cpu->Regs()[15] << "\n"
                  << "  CPSR = 0x" << cpu->Cpsr() << std::dec << "\n";
    }

    std::optional<uint32_t> MemoryReadCode(uint32_t vaddr) override {
        try {
            return MemoryRead32(vaddr);
        } catch (const std::exception& e) {
            std::cerr << "[Tracer] CPU failed to fetch instruction at 0x" << std::hex << vaddr
                      << " (" << e.what() << ")" << std::dec << std::endl;
            throw;
        }
    }

    void PrintOOBMemoryRead(uint32_t vaddr, std::string funcName) {
        bool is_oob = (vaddr < 0x40000000) ||
                      (vaddr == 0xfffffff4 || vaddr == 0xfffffff8 || vaddr == 0xfffffffc);
        if (!is_oob) return;

        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n[CRASH TRAP] Caught OOB reading " << funcName <<  " 0x"
                  << std::hex << vaddr << std::dec << "!" << std::endl;
        if (cpu) {
            for (int r = 0; r < 13; r++)
                std::cout << "    R" << r << "=0x" << std::hex << cpu->Regs()[r] << std::dec << "\n";
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "host_tid=" << Watchpoint::TidString(std::this_thread::get_id()) << std::endl;
            // Dump allocations + watched ranges around the crashing SP so we can see
            // whether the live stack frame at SP overlaps any tracked heap block.
            mem.DumpAllocationsNear(cpu->Regs()[13]);
        }
        // Violently kill the emulator before it can slide!
        std::exit(1);
    }

    void CallSVC(uint32_t swi) override {
        // --- pthread_once_done detection ---
        if (swi == 0xFFFFFF) {
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            return;
        }

        std::string name = loader.GetSymbolName(swi);
        router.Invoke(name, cpu);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        std::cerr << "[CPU] Exception at 0x" << std::hex << pc << std::dec 
                  << " (Type: " << static_cast<int>(exception) << ")" << std::endl;
    }

    void InterpreterFallback(uint32_t pc, size_t num_instructions) override {
        // Called when Dynarmic can't JIT an instruction. Logging this is useful for debugging.
        std::cerr << "[CPU] InterpreterFallback at 0x" << std::hex << pc
                  << " (count=" << num_instructions << ")" << std::dec << std::endl;
    }

    void AddTicks(uint64_t ticks) override {}
    uint64_t GetTicksRemaining() override { return 100000; }

    Dynarmic::A32::Jit* cpu = nullptr;

private:
    GuestMemory& mem;
    ElfLoader& loader;
    SyscallRouter& router;
};

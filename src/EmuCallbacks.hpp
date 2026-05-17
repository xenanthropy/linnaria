#pragma once
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <string>
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
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
    void MemoryWrite8(uint32_t vaddr, uint8_t value) override { *mem.GetHostPointer(vaddr) = value; }

    uint16_t MemoryRead16(uint32_t vaddr) override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead16)");
        return MemoryRead8(vaddr) | (uint16_t(MemoryRead8(vaddr + 1)) << 8);
    }
    void MemoryWrite16(uint32_t vaddr, uint16_t value) override {
        MemoryWrite8(vaddr,     value & 0xFF);
        MemoryWrite8(vaddr + 1, (value >> 8) & 0xFF);
    }

    uint32_t MemoryRead32(uint32_t vaddr)  override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead32)");
        return mem.Read32(vaddr);
    }
    void MemoryWrite32(uint32_t vaddr, uint32_t value)  override { mem.Write32(vaddr, value); }

    uint64_t MemoryRead64(uint32_t vaddr)  override {
        PrintOOBMemoryRead(vaddr, "(MemoryRead64)");
        return uint64_t(MemoryRead32(vaddr)) | (uint64_t(MemoryRead32(vaddr + 4)) << 32);
    }
    void MemoryWrite64(uint32_t vaddr, uint64_t value) override {
        MemoryWrite32(vaddr,     uint32_t(value));
        MemoryWrite32(vaddr + 4, uint32_t(value >> 32));
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
        if (vaddr < 0x40000000) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\n[CRASH TRAP] Caught OOB reading " << funcName <<  " 0x" << std::hex << vaddr << std::dec << "!" << std::endl;
            if (cpu) {
                std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
                std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
                std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
                std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
                std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
                std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
                std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
                std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            }
            // Violently kill the emulator before it can slide!
            std::exit(1);
        }

        if (vaddr == 0xfffffff4 || vaddr == 0xfffffff8 || vaddr == 0xfffffffc) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\n[CRASH TRAP] Caught OOB reading " << funcName << " 0x" << std::hex << vaddr << std::dec << "!" << std::endl;
            if (cpu) {
                std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
                std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
                std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
                std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
                std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
                std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
                std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
                std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;                
            }
            // Violently kill the emulator before it can slide!
            std::exit(1);
        }
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

#pragma once
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"

class EmuCallbacks : public Dynarmic::A32::UserCallbacks {
public:
    EmuCallbacks(GuestMemory& mem, ElfLoader& loader, SyscallRouter& router)
        : mem(mem), loader(loader), router(router) {}

    // --- 8/16/32/64-bit memory ---
    uint8_t  MemoryRead8(uint32_t vaddr)  override { return *mem.GetHostPointer(vaddr); }
    void     MemoryWrite8(uint32_t vaddr, uint8_t value) override { *mem.GetHostPointer(vaddr) = value; }

    uint16_t MemoryRead16(uint32_t vaddr) override {
        return MemoryRead8(vaddr) | (uint16_t(MemoryRead8(vaddr + 1)) << 8);
    }
    void MemoryWrite16(uint32_t vaddr, uint16_t value) override {
        MemoryWrite8(vaddr,     value & 0xFF);
        MemoryWrite8(vaddr + 1, (value >> 8) & 0xFF);
    }

    uint32_t MemoryRead32(uint32_t vaddr)  override { return mem.Read32(vaddr); }
    void     MemoryWrite32(uint32_t vaddr, uint32_t value)  override { mem.Write32(vaddr, value); }

    uint64_t MemoryRead64(uint32_t vaddr)  override {
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

    void CallSVC(uint32_t swi) override {
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

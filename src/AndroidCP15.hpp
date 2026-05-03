#pragma once
#include <array>
#include <dynarmic/interface/A32/coprocessor.h>
#include <optional>
#include <cstdint>
#include "GuestMemory.hpp"

class AndroidCP15 : public Dynarmic::A32::Coprocessor {
    using CoprocReg = Dynarmic::A32::CoprocReg;
public:
    // A harmless dummy callback for complex operations so Dynarmic never panics
    static std::uint64_t DummyCallback(void*, std::uint32_t, std::uint32_t) {
        return 0; 
    }

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { 
        return Callback{ &DummyCallback, nullptr }; 
    }
    
    // MCR (Write to Coprocessor)
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override {
        return &dummy_value;
    }
    
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return std::array<std::uint32_t*, 2>{&dummy_value, &dummy_value};
    }
    
    // MRC (Read from Coprocessor)
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        // Intercept the Android Thread Local Storage register
        if (!two && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            tls_pointer = GuestMemory::CODE_BASE + 0x2000000; 
            return &tls_pointer;
        }
        
        // Swallow everything else with dummy data
        return &dummy_value;
    }
    
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        return std::array<std::uint32_t*, 2>{&dummy_value, &dummy_value};
    }
    
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { 
        return Callback{ &DummyCallback, nullptr }; 
    }
    
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { 
        return Callback{ &DummyCallback, nullptr }; 
    }

private:
    std::uint32_t tls_pointer = 0;
    std::uint32_t dummy_value = 0;
};

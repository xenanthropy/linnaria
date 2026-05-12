#pragma once
#include <array>
#include <dynarmic/interface/A32/coprocessor.h>
#include <optional>
#include <cstdint>
#include "GuestMemory.hpp"

class AndroidCP15 : public Dynarmic::A32::Coprocessor {
    using CoprocReg = Dynarmic::A32::CoprocReg;
public:
    // Pass the Thread Local Storage pointer via constructor
    AndroidCP15(uint32_t tls_addr) : tls_pointer(tls_addr) {}
    
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
    
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc1 == 0 && opc2 == 3) {
            // Return the unique thread pointer instead of a hardcoded one
            return &tls_pointer; 
        }
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

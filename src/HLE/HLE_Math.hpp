#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <unistd.h>
#include <math.h>

namespace HLE::Math {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        //TODO: acosf, asinf, atan2f, cos, sin, tanf, exp, fabsf, ceil, frexp, modf, modff
        //      log, pow, powf

        ROUTE_REGISTER(router, "cosf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::cos(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        ROUTE_REGISTER(router, "sinf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::sin(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        ROUTE_REGISTER(router, "floor", [](Dynarmic::A32::Jit* cpu) {
            uint64_t raw_in = static_cast<uint64_t>(cpu->Regs()[0]) | (static_cast<uint64_t>(cpu->Regs()[1]) << 32);
            double d_in;
            std::memcpy(&d_in, &raw_in, sizeof(double));
            
            double d_out = std::floor(d_in);
            
            uint64_t raw_out;
            std::memcpy(&raw_out, &d_out, sizeof(double));
            cpu->Regs()[0] = static_cast<uint32_t>(raw_out & 0xFFFFFFFF);
            cpu->Regs()[1] = static_cast<uint32_t>(raw_out >> 32);
        });

        ROUTE_REGISTER(router, "floorf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            float f_out = std::floor(f_in);
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        ROUTE_REGISTER(router, "ceilf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            float f_out = std::ceil(f_in);
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        ROUTE_REGISTER(router, "sqrt", [](Dynarmic::A32::Jit* cpu) {
            // doubles take up two 32-bit registers (R0 and R1)
            uint64_t raw_in = static_cast<uint64_t>(cpu->Regs()[0]) | (static_cast<uint64_t>(cpu->Regs()[1]) << 32);
            double d_in;
            std::memcpy(&d_in, &raw_in, sizeof(double));
            
            double d_out = std::sqrt(d_in);
            
            uint64_t raw_out;
            std::memcpy(&raw_out, &d_out, sizeof(double));
            cpu->Regs()[0] = static_cast<uint32_t>(raw_out & 0xFFFFFFFF);
            cpu->Regs()[1] = static_cast<uint32_t>(raw_out >> 32);
        });

        ROUTE_REGISTER(router, "sqrtf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::sqrt(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        ROUTE_REGISTER(router, "fmodf",[](Dynarmic::A32::Jit* cpu) {
            float x, y;
            std::memcpy(&x, &cpu->Regs()[0], 4);
            std::memcpy(&y, &cpu->Regs()[1], 4);
            float result = std::fmod(x, y);
            std::memcpy(&cpu->Regs()[0], &result, 4);
        });

    }
}

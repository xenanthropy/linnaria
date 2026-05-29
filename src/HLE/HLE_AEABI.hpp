#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <cstring>
#include <cstdint>

// Bionic-provided softfp runtime routines. The library is built ARMv5TE with
// soft-float ABI: doubles ride in r0:r1 (low:high) and r2:r3, single floats
// in r0, results returned in r0[:r1]. The host has full SSE2 hardware FP, so
// we just bit-cast through native double / float ops and skip the ARM
// runtime entirely.
//
// Why these matter: without HLE handlers, every PLT import lands in the
// [UNIMPLEMENTED] path returning 0 -- so every double-precision add/sub
// and every int->double conversion that resolved through the PLT has been
// silently producing zero. Probably invisible during play (cosmetic curves
// and biome blending), but real.
//
// The PLT import set for libTerraria 1.2.12785 is small (7 routines below):
// most aeabi soft-float lives statically linked inside .text and would need
// a different intercept mechanism (entry-point patching) to replace.

namespace HLE::AEABI {

    namespace {
        // double <-> r0:r1 bit-cast helpers. low half in r0, high half in r1
        // per AAPCS softfp register pairing.
        inline double DoubleFromRegs(uint32_t lo, uint32_t hi) {
            uint64_t bits = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
            double d;
            std::memcpy(&d, &bits, sizeof(double));
            return d;
        }

        inline void DoubleToRegs(double d, Dynarmic::A32::Jit* cpu) {
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(double));
            cpu->Regs()[0] = static_cast<uint32_t>(bits);
            cpu->Regs()[1] = static_cast<uint32_t>(bits >> 32);
        }
    }

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {
        (void)memory;

        // double __aeabi_dadd(double a, double b)
        //   a -> r0:r1, b -> r2:r3, return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_dadd", [](Dynarmic::A32::Jit* cpu) {
            double a = DoubleFromRegs(cpu->Regs()[0], cpu->Regs()[1]);
            double b = DoubleFromRegs(cpu->Regs()[2], cpu->Regs()[3]);
            DoubleToRegs(a + b, cpu);
        });

        // double __aeabi_dsub(double a, double b)
        //   a -> r0:r1, b -> r2:r3, return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_dsub", [](Dynarmic::A32::Jit* cpu) {
            double a = DoubleFromRegs(cpu->Regs()[0], cpu->Regs()[1]);
            double b = DoubleFromRegs(cpu->Regs()[2], cpu->Regs()[3]);
            DoubleToRegs(a - b, cpu);
        });

        // double __aeabi_f2d(float a)
        //   a -> r0 (bit-cast), return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_f2d", [](Dynarmic::A32::Jit* cpu) {
            uint32_t bits = cpu->Regs()[0];
            float f;
            std::memcpy(&f, &bits, sizeof(float));
            DoubleToRegs(static_cast<double>(f), cpu);
        });

        // double __aeabi_i2d(int a)
        //   a -> r0, return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_i2d", [](Dynarmic::A32::Jit* cpu) {
            int32_t i = static_cast<int32_t>(cpu->Regs()[0]);
            DoubleToRegs(static_cast<double>(i), cpu);
        });

        // double __aeabi_ui2d(unsigned int a)
        //   a -> r0, return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_ui2d", [](Dynarmic::A32::Jit* cpu) {
            uint32_t u = cpu->Regs()[0];
            DoubleToRegs(static_cast<double>(u), cpu);
        });

        // double __aeabi_l2d(long long a)
        //   a -> r0:r1 (low:high), return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_l2d", [](Dynarmic::A32::Jit* cpu) {
            uint64_t bits = static_cast<uint64_t>(cpu->Regs()[0])
                          | (static_cast<uint64_t>(cpu->Regs()[1]) << 32);
            int64_t i = static_cast<int64_t>(bits);
            DoubleToRegs(static_cast<double>(i), cpu);
        });

        // double __aeabi_ul2d(unsigned long long a)
        //   a -> r0:r1 (low:high), return -> r0:r1
        ROUTE_REGISTER(router, "__aeabi_ul2d", [](Dynarmic::A32::Jit* cpu) {
            uint64_t u = static_cast<uint64_t>(cpu->Regs()[0])
                       | (static_cast<uint64_t>(cpu->Regs()[1]) << 32);
            DoubleToRegs(static_cast<double>(u), cpu);
        });
    }

}

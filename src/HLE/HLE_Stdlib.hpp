#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"

namespace HLE::Stdlib {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        //TODO: qsort

        ROUTE_REGISTER(router, "srand48", [](Dynarmic::A32::Jit* cpu) {
            long int seed = static_cast<long int>(cpu->Regs()[0]);
            srand48(seed);
        });

        ROUTE_REGISTER(router, "lrand48",[](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = static_cast<uint32_t>(lrand48());
        });

        ROUTE_REGISTER(router, "printf", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t fmt_ptr = cpu->Regs()[0];
            if (!fmt_ptr) { cpu->Regs()[0] = 0; return; }

            const char* fmt = reinterpret_cast<const char*>(memory.GetHostPointer(fmt_ptr));
            uint32_t sp = cpu->Regs()[13];
            int reg = 1; // next argument register (R1, R2, R3)

            auto next_u32 = [&]() -> uint32_t {
                if (reg <= 3) return cpu->Regs()[reg++];
                uint32_t v = memory.Read32(sp);
                sp += 4;
                return v;
            };

            std::string out;
            for (size_t i = 0; fmt[i]; ++i) {
                if (fmt[i] != '%' || fmt[i+1] == '\0') {
                    out += fmt[i];
                    continue;
                }
                char spec = fmt[++i];

                if (spec == '%') { out += '%'; continue; }

                // %lld / %llu
                if (spec == 'l' && fmt[i+1] == 'l') {
                    i++; // skip second 'l'
                    char ll_spec = fmt[++i];
                    uint32_t lo = next_u32();
                    uint32_t hi = next_u32();
                    if (ll_spec == 'd' || ll_spec == 'i') {
                        int64_t v = static_cast<int64_t>((uint64_t)lo | ((uint64_t)hi << 32));
                        out += std::to_string(v);
                    } else if (ll_spec == 'u') {
                        uint64_t v = (uint64_t)lo | ((uint64_t)hi << 32);
                        out += std::to_string(v);
                    } else {
                        char buf[32]; std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)lo | ((unsigned long long)hi << 32));
                        out += buf;
                    }
                    continue;
                }

                if (spec == 's') {
                    uint32_t p = next_u32();
                    out += p ? reinterpret_cast<const char*>(memory.GetHostPointer(p)) : "(null)";
                } else if (spec == 'd' || spec == 'i') {
                    out += std::to_string(static_cast<int32_t>(next_u32()));
                } else if (spec == 'u') {
                    out += std::to_string(static_cast<uint32_t>(next_u32()));
                } else if (spec == 'x' || spec == 'X') {
                    char buf[16]; std::snprintf(buf, sizeof(buf), "%x", next_u32()); out += buf;
                } else if (spec == 'p') {
                    char buf[16]; std::snprintf(buf, sizeof(buf), "0x%x", next_u32()); out += buf;
                } else if (spec == 'c') {
                    out += static_cast<char>(next_u32());
                } else if (spec == 'f' || spec == 'g' || spec == 'e') {
                    // On ARM variadic ABI, double consumes two aligned 32-bit slots.
                    // We don't enforce even alignment here; for simple int-only prints it's fine.
                    uint32_t lo = next_u32();
                    uint32_t hi = next_u32();
                    uint64_t raw = (uint64_t)lo | ((uint64_t)hi << 32);
                    double d; std::memcpy(&d, &raw, sizeof(d));
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%f", d); out += buf;
                } else {
                    out += '%'; out += spec;
                }
            }
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[Guest printf] " << out << std::endl;
            }
            cpu->Regs()[0] = static_cast<int>(out.size());
        });


    }

}

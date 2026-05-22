#pragma once
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "EmuCallbacks.hpp"

namespace HLE::Stdlib {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory, ElfLoader& loader) {

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

        ROUTE_REGISTER(router, "qsort", [&memory, &loader, &router](Dynarmic::A32::Jit* cpu) {
            uint32_t base   = cpu->Regs()[0];
            uint32_t nmemb  = cpu->Regs()[1];
            uint32_t size   = cpu->Regs()[2];
            uint32_t compar = cpu->Regs()[3];

            if (nmemb <= 1 || size == 0) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Spin up a temporary lightweight CPU instance specifically for the comparator
            // to avoid violating Dynarmic's non-reentrant Run() rule.
            Dynarmic::A32::UserConfig config;
            EmuCallbacks temp_callbacks(memory, loader, router);
            config.callbacks = &temp_callbacks;
            config.page_table = &memory.page_table;
            config.absolute_offset_page_table = false;
            config.fastmem_pointer = Config::Performance::fastmem
                ? reinterpret_cast<uintptr_t>(memory.fastmem_base) : 0;
            config.recompile_on_fastmem_failure = true;
            config.arch_version = Dynarmic::A32::ArchVersion::v7;
            config.processor_id = 999; // Unique ID so it doesn't collide with hardware cores

            Dynarmic::A32::Jit temp_cpu(config);
            temp_callbacks.cpu = &temp_cpu;

            // Allocate a safe scratch stack for the temporary CPU
            uint32_t temp_sp = memory.AllocateHeap(1024 * 64);

            // Create an array of indices to sort so we don't have to swap guest memory during the algorithm
            std::vector<uint32_t> indices(nmemb);
            for (uint32_t i = 0; i < nmemb; i++) indices[i] = i;

            auto compare_elements = [&](uint32_t idx_a, uint32_t idx_b) -> bool {
                temp_cpu.Regs()[13] = temp_sp + (1024 * 64);
                temp_cpu.Regs()[0]  = base + idx_a * size;
                temp_cpu.Regs()[1]  = base + idx_b * size;
                temp_cpu.Regs()[14] = loader.GetThunk("Emulator_Return_Trap");

                uint32_t cpsr = 0x10; // User mode
                if (compar & 1) cpsr |= 0x20; // Thumb mode
                temp_cpu.SetCpsr(cpsr);
                temp_cpu.Regs()[15] = compar & ~1;

                while (true) {
                    auto halt = temp_cpu.Run();
                    if (halt == Dynarmic::HaltReason::UserDefined1) break;
                    if (static_cast<uint64_t>(halt) != 0) temp_cpu.ClearHalt(halt);
                }

                // qsort expects < 0 for a < b
                int result = static_cast<int>(temp_cpu.Regs()[0]);
                return result < 0;
            };

            std::sort(indices.begin(), indices.end(), compare_elements);

            // Read out the memory in the newly sorted order
            std::vector<uint8_t> sorted_data(nmemb * size);
            for (uint32_t i = 0; i < nmemb; i++) {
                uint32_t src = base + indices[i] * size;
                for (uint32_t j = 0; j < size; j++) {
                    sorted_data[i * size + j] = memory.Read8(src + j);
                }
            }

            // Write it sequentially back into the guest
            for (uint32_t i = 0; i < nmemb * size; i++) {
                memory.Write8(base + i, sorted_data[i]);
            }

            memory.FreeHeap(temp_sp);
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "strtol", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nptr_ptr    = cpu->Regs()[0];
            uint32_t endptr_ptr  = cpu->Regs()[1];
            int      base        = static_cast<int>(cpu->Regs()[2]);

            const char* nptr = reinterpret_cast<const char*>(memory.GetHostPointer(nptr_ptr));

            char* endptr = nullptr;
            long result = std::strtol(nptr, &endptr, base);

            cpu->Regs()[0] = static_cast<uint32_t>(result);

            // If endptr_ptr is provided, write the guest address of the first invalid character
            if (endptr_ptr != 0 && nptr_ptr != 0 && endptr != nullptr) {
                uint32_t end_guest = nptr_ptr + static_cast<uint32_t>(endptr - nptr);
                memory.Write32(endptr_ptr, end_guest);
            } else if (endptr_ptr != 0) {
                memory.Write32(endptr_ptr, 0);
            }
        });

        ROUTE_REGISTER(router, "strtoul", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nptr_ptr    = cpu->Regs()[0];
            uint32_t endptr_ptr  = cpu->Regs()[1];
            int      base        = static_cast<int>(cpu->Regs()[2]);

            const char* nptr = reinterpret_cast<const char*>(memory.GetHostPointer(nptr_ptr));
            char* endptr = nullptr;
            unsigned long result = std::strtoul(nptr, &endptr, base);

            cpu->Regs()[0] = static_cast<uint32_t>(result);
            if (endptr_ptr != 0 && nptr_ptr != 0 && endptr != nullptr) {
                memory.Write32(endptr_ptr, nptr_ptr + (endptr - nptr));
            } else if (endptr_ptr != 0) {
                memory.Write32(endptr_ptr, 0);
            }
        });

    }
}

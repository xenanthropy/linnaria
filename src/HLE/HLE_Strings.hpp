#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <mutex>

namespace HLE::Strings {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        ROUTE_REGISTER(router, "strlen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            bool is_xml = (0x55b9dbc0 != 0 && ptr >= 0x55b9dbc0 && ptr < (0x55b9dbc0 + 0x223b));

            uint32_t len = 0;
            // Safely compute length (limit to 1MB to avoid hangs)
            while (len < 1024*1024 && memory.Read8(ptr + len) != '\0') {
                len++;
            }

            cpu->Regs()[0] = len;
        });

        ROUTE_REGISTER(router, "strcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t s1 = cpu->Regs()[0];
            uint32_t s2 = cpu->Regs()[1];
            const char* str1 = reinterpret_cast<const char*>(memory.GetHostPointer(s1));
            const char* str2 = reinterpret_cast<const char*>(memory.GetHostPointer(s2));

            /* IGNORE: Debug print
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[Router] strcmp: comparing '" << (str1 ? str1 : "(null)") 
                          << "' with '" << (str2 ? str2 : "(null)") << "'" << std::endl;
            }
            */

            cpu->Regs()[0] = std::strcmp(str1, str2);
        });

        ROUTE_REGISTER(router, "strncmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t s1 = cpu->Regs()[0];
            uint32_t s2 = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];

            int result = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c1 = memory.Read8(s1 + i);
                uint8_t c2 = memory.Read8(s2 + i);
                if (c1 != c2 || c1 == '\0') {
                    result = c1 - c2;
                    break;
                }
            }
            cpu->Regs()[0] = result;
        });

        ROUTE_REGISTER(router, "strrchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];

            const char* str = reinterpret_cast<const char*>(memory.GetHostPointer(ptr));
            const char* res = std::strrchr(str, ch);

            if (res) cpu->Regs()[0] = ptr + (res - str);
            else cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "vsprintf", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            uint32_t fmt_ptr = cpu->Regs()[1];
            uint32_t ap      = cpu->Regs()[2]; // va_list (guest pointer to packed args)

            std::string format;
            {
                uint32_t f = fmt_ptr;
                while (char c = static_cast<char>(memory.Read8(f++))) format += c;
            }

            // 32-bit arg from the va_list. AAPCS soft-float passes a va_list as a
            // pointer to a contiguous arg area; each int/ptr/float-as-double slot
            // is the same as on-stack varargs.
            auto get_next_arg_32 = [&]() -> uint32_t {
                uint32_t v = memory.Read32(ap);
                ap += 4;
                return v;
            };

            // 64-bit args (double / %lld) require 8-byte alignment in the va_list.
            auto get_next_arg_64 = [&]() -> uint64_t {
                if (ap % 8 != 0) ap += 4;
                uint32_t low  = memory.Read32(ap); ap += 4;
                uint32_t high = memory.Read32(ap); ap += 4;
                return (static_cast<uint64_t>(high) << 32) | low;
            };

            std::string result;
            for (size_t i = 0; i < format.length(); i++) {
                if (format[i] == '%' && i + 1 < format.length() && format[i + 1] != '%') {
                    size_t j = i + 1;
                    // Skip past flags, width, precision, length modifiers
                    while (j < format.length()
                           && std::string("cdiouxXfFeEgGspn").find(format[j]) == std::string::npos) {
                        j++;
                    }
                    if (j < format.length()) {
                        char type = format[j];
                        std::string specifier = format.substr(i, j - i + 1);
                        char temp_buf[512];

                        if (type == 'f' || type == 'F' || type == 'e' || type == 'E'
                            || type == 'g' || type == 'G') {
                            uint64_t val = get_next_arg_64();
                            double d;
                            std::memcpy(&d, &val, sizeof(double));
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), d);
                        } else if (type == 's') {
                            uint32_t s_ptr = get_next_arg_32();
                            std::string s;
                            if (s_ptr) {
                                uint32_t c = s_ptr;
                                while (char ch = static_cast<char>(memory.Read8(c++))) s += ch;
                            } else {
                                s = "(null)";
                            }
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), s.c_str());
                        } else {
                            uint32_t val = get_next_arg_32();
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), val);
                        }

                        result += temp_buf;
                        i = j;
                        continue;
                    }
                } else if (format[i] == '%' && i + 1 < format.length() && format[i + 1] == '%') {
                    result += '%';
                    i++;
                    continue;
                }
                result += format[i];
            }

            char* out_str = reinterpret_cast<char*>(memory.GetHostPointer(str_ptr));
            std::strcpy(out_str, result.c_str());
            cpu->Regs()[0] = static_cast<uint32_t>(result.length());
        });

        ROUTE_REGISTER(router, "atoi", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            if (str_ptr != 0) {
                std::string str = reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr));
                cpu->Regs()[0] = std::atoi(str.c_str());
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "strtod", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nptr        = cpu->Regs()[0];
            uint32_t endptr_ptr  = cpu->Regs()[1];

            // Safely map the guest string
            // If null, feed an empty string to strtod
            const char* host_str = "";
            if (nptr) {
                host_str = reinterpret_cast<const char*>(memory.GetHostPointer(nptr));
            }

            // Call host strtod
            char* host_end = nullptr;
            double result = std::strtod(host_str, &host_end);

            // Write back the guest address of the first invalid character if endptr was provided
            if (endptr_ptr) {
                if (nptr && host_end) {
                    uint32_t end_guest = nptr + static_cast<uint32_t>(host_end - host_str);
                    memory.Write32(endptr_ptr, end_guest);
                } else {
                    memory.Write32(endptr_ptr, 0);
                }
            }

            // Return raw double bits in R0:R1 (ARM 32-bit softfp/soft-float ABI)
            uint64_t raw;
            std::memcpy(&raw, &result, sizeof(double));
            cpu->Regs()[0] = static_cast<uint32_t>(raw & 0xFFFFFFFF);
            cpu->Regs()[1] = static_cast<uint32_t>(raw >> 32);
        });

        ROUTE_REGISTER(router, "wcslen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            const wchar_t* str = reinterpret_cast<const wchar_t*>(memory.GetHostPointer(str_ptr));
            cpu->Regs()[0] = std::wcslen(str);
        });

        ROUTE_REGISTER(router, "wctob", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c < 128) ? c : -1; // -1 is EOF
        });

        ROUTE_REGISTER(router, "btowc", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c != (uint32_t)-1) ? c : -1; // WEOF
        });

        ROUTE_REGISTER(router, "wctype", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 1; // Return a valid property ID
        });

        ROUTE_REGISTER(router, "towlower", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
        });

        ROUTE_REGISTER(router, "sprintf", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t buf_ptr = cpu->Regs()[0];
            uint32_t fmt_ptr = cpu->Regs()[1];

            // Read the format string
            std::string format;
            uint32_t f_curr = fmt_ptr;
            while (char c = memory.Read8(f_curr++)) format += c;

            std::string result;
            uint32_t arg_idx = 0; // Tracks our position in the variadic argument list

            // Helper to fetch the next 32-bit argument
            auto get_next_arg_32 = [&]() -> uint32_t {
                if (arg_idx == 0) { arg_idx++; return cpu->Regs()[2]; }
                if (arg_idx == 1) { arg_idx++; return cpu->Regs()[3]; }
                uint32_t sp = cpu->Regs()[13];
                uint32_t val = memory.Read32(sp + (arg_idx - 2) * 4);
                arg_idx++;
                return val;
            };

            // Helper to fetch the next 64-bit argument (forces 8-byte alignment)
            auto get_next_arg_64 = [&]() -> uint64_t {
                if (arg_idx % 2 != 0) arg_idx++; 
                uint32_t low = get_next_arg_32();
                uint32_t high = get_next_arg_32();
                return (static_cast<uint64_t>(high) << 32) | low;
            };

            // Parse the format string manually
            for (size_t i = 0; i < format.length(); i++) {
                if (format[i] == '%' && i + 1 < format.length() && format[i+1] != '%') {
                    size_t j = i + 1;

                    // Skip past width, precision, and length modifiers to find the type
                    while (j < format.length() && std::string("cdiouxXfFeEgGspn").find(format[j]) == std::string::npos) {
                        j++;
                    }

                    if (j < format.length()) {
                        char type = format[j];
                        std::string specifier = format.substr(i, j - i + 1);
                        char temp_buf[512];

                        if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == 'g' || type == 'G') {
                            uint64_t val = get_next_arg_64();
                            double d;
                            std::memcpy(&d, &val, sizeof(double));
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), d);
                        } else if (type == 's') {
                            uint32_t str_ptr = get_next_arg_32();
                            std::string s;
                            if (str_ptr) {
                                uint32_t s_curr = str_ptr;
                                while (char c = memory.Read8(s_curr++)) s += c;
                            } else {
                                s = "(null)";
                            }
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), s.c_str());
                        } else {
                            // Integer and pointer types
                            uint32_t val = get_next_arg_32();
                            snprintf(temp_buf, sizeof(temp_buf), specifier.c_str(), val);
                        }

                        result += temp_buf;
                        i = j;
                        continue;
                    }
                } else if (format[i] == '%' && i + 1 < format.length() && format[i+1] == '%') {
                    result += '%';
                    i++;
                    continue;
                }
                result += format[i];
            }

            // Write the compiled string back to the guest buffer
            for (size_t i = 0; i < result.length(); i++) {
                memory.Write8(buf_ptr + i, result[i]);
            }
            memory.Write8(buf_ptr + result.length(), 0);

            cpu->Regs()[0] = result.length(); // Return number of characters written
        });

    }
}

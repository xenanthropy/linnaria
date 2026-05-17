#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "HostAssetManager.hpp"
#include <mutex>

static std::mutex asset_manager_lock;

namespace HLE::Android {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        // TODO:

        ROUTE_REGISTER(router, "Emulator_Return_Trap", [](Dynarmic::A32::Jit* cpu) {
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined1); 
        });
        
        // --- Asset Manager ---
        ROUTE_REGISTER(router, "AAssetManager_fromJava", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = HostAssetManager::HANDLE_ID;
        });

        ROUTE_REGISTER(router, "AAssetManager_open", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(asset_manager_lock);
            uint32_t mgr_ptr = cpu->Regs()[0];
            uint32_t filename_ptr = cpu->Regs()[1];
            if (mgr_ptr != HostAssetManager::HANDLE_ID) {
                cpu->Regs()[0] = 0;
                return;
            }

            std::string filename;
            char c;
            uint32_t offset = 0;
            while ((c = memory.Read8(filename_ptr + offset)) != '\0') {
                filename += c;
                offset++;
            }

            std::string full_path = host_assets.base_path + filename;
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[AssetManager] Opening: " << full_path << std::endl;
            }
            auto& asset = host_assets.open_files[host_assets.next_fd];
            asset.file.open(full_path, std::ios::binary | std::ios::ate);

            if (!asset.file.is_open()) {
                {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[AssetManager] FAILED to open: " << full_path << std::endl;
                }
                host_assets.open_files.erase(host_assets.next_fd);
                cpu->Regs()[0] = 0;
                return;
            }

            // CRITICAL: these must run for BOTH normal and fallback paths
            asset.length = asset.file.tellg();
            asset.file.seekg(0, std::ios::beg);
            cpu->Regs()[0] = host_assets.next_fd++;
        });

        ROUTE_REGISTER(router, "AAsset_read", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(asset_manager_lock);
            uint32_t asset_ptr = cpu->Regs()[0];
            uint32_t buffer_ptr = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            if (host_assets.open_files.find(asset_ptr) == host_assets.open_files.end()) {
                cpu->Regs()[0] = 0; 
                return;
            }

            auto& asset = host_assets.open_files[asset_ptr];
            
            std::vector<char> temp_buf(count);
            asset.file.read(temp_buf.data(), count);
            uint32_t bytes_read = asset.file.gcount();

            for (uint32_t i = 0; i < bytes_read; i++) {
                memory.Write8(buffer_ptr + i, temp_buf[i]);
            }

            cpu->Regs()[0] = bytes_read; 
        });

        ROUTE_REGISTER(router, "AAsset_close", [](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(asset_manager_lock);
            uint32_t asset_ptr = cpu->Regs()[0];
            host_assets.open_files.erase(asset_ptr);
        });

        ROUTE_REGISTER(router, "AAsset_getLength", [](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(asset_manager_lock);
            uint32_t asset_ptr = cpu->Regs()[0];
            if (host_assets.open_files.find(asset_ptr) != host_assets.open_files.end()) {
                cpu->Regs()[0] = host_assets.open_files[asset_ptr].length;
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "AAsset_getBuffer", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(asset_manager_lock);
            uint32_t asset_ptr = cpu->Regs()[0];
            if (host_assets.open_files.find(asset_ptr) == host_assets.open_files.end()) {
                cpu->Regs()[0] = 0; 
                return;
            }

            auto& asset = host_assets.open_files[asset_ptr];
            
            // If we haven't loaded this file into guest memory yet, do it now
            if (asset.buffer_ptr == 0) {
                asset.buffer_ptr = memory.AllocateHeap(asset.length + 1); // +1 for null terminator
                
                // Read the whole file into a temporary host buffer
                asset.file.seekg(0, std::ios::beg);
                std::vector<char> temp_buf(asset.length);
                asset.file.read(temp_buf.data(), asset.length);
                
                // Copy it into guest memory
                std::memcpy(memory.GetHostPointer(asset.buffer_ptr), temp_buf.data(), asset.length);
                memory.Write8(asset.buffer_ptr + asset.length, 0); // Safely null terminate

                // debug print
                {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[AssetManager] -> Allocated " << asset.length << " bytes for guest buffer." << std::endl;
                }
                if (asset.length >= 4) {
                    uint8_t* magic = memory.GetHostPointer(asset.buffer_ptr);
                    {
                        std::lock_guard<std::mutex> lock(console_mutex);                    
                        printf("[AssetManager] -> Magic Bytes: %02X %02X %02X %02X\n", magic[0], magic[1], magic[2], magic[3]);
                    }
                }
            }
            
            cpu->Regs()[0] = asset.buffer_ptr;
        });

        ROUTE_REGISTER(router, "__android_log_print", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t tag_ptr = cpu->Regs()[1];
            uint32_t fmt_ptr = cpu->Regs()[2];
            
            std::string tag = reinterpret_cast<const char*>(memory.GetHostPointer(tag_ptr));
            std::string fmt = reinterpret_cast<const char*>(memory.GetHostPointer(fmt_ptr));
            
            uint32_t current_arg_reg = 3;
            uint32_t current_stack_ptr = cpu->Regs()[13]; 
            
            // Standard 32-bit fetch
            auto get_next_arg = [&]() -> uint32_t {
                if (current_arg_reg <= 3) {
                    return cpu->Regs()[current_arg_reg++];
                } else {
                    uint32_t val = memory.Read32(current_stack_ptr);
                    current_stack_ptr += 4;
                    return val;
                }
            };

            // 64-bit fetch for doubles (Automatically skips odd registers for 8-byte alignment)
            auto get_next_arg_64 = [&]() -> uint64_t {
                if (current_arg_reg == 3) current_arg_reg++; // Skip R3
                if (current_stack_ptr % 8 != 0) current_stack_ptr += 4; // Align Stack
                
                uint64_t low = memory.Read32(current_stack_ptr);
                uint64_t high = memory.Read32(current_stack_ptr + 4);
                current_stack_ptr += 8;
                return low | (high << 32);
            };

            std::string output;
            for (size_t i = 0; i < fmt.length(); i++) {
                if (fmt[i] == '%' && i + 1 < fmt.length()) {
                    i++;
                    if (fmt[i] == '%') {
                        output += '%';
                        continue;
                    }
                    
                    // Parse formatting modifiers (e.g., the "2.2" in "%2.2f")
                    std::string format_spec = "%";
                    while (i < fmt.length() && (isdigit(fmt[i]) || fmt[i] == '.' || fmt[i] == '-' || fmt[i] == '+')) {
                        format_spec += fmt[i];
                        i++;
                    }
                    
                    if (i < fmt.length()) {
                        format_spec += fmt[i]; 
                        char type = fmt[i];
                        
                        char buf[256];
                        if (type == 'f') {
                            uint64_t raw_double = get_next_arg_64();
                            double d;
                            std::memcpy(&d, &raw_double, sizeof(double));
                            snprintf(buf, sizeof(buf), format_spec.c_str(), d);
                            output += buf;
                        } else if (type == 'p') {
                            snprintf(buf, sizeof(buf), "0x%08X", get_next_arg());
                            output += buf;
                        } else if (type == 's') {
                            uint32_t str_ptr = get_next_arg();
                            if (str_ptr) output += reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr));
                            else output += "(null)";
                        } else if (type == 'd' || type == 'i' || type == 'x' || type == 'X' || type == 'u') {
                            snprintf(buf, sizeof(buf), format_spec.c_str(), get_next_arg());
                            output += buf;
                        } else {
                            output += format_spec; // Fallback
                        }
                    }
                } else {
                    output += fmt[i];
                }
            }
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[Android Log] " << tag << ": " << output << std::endl;
            }
            cpu->Regs()[0] = 0;
        });        

        ROUTE_REGISTER(router, "__gnu_Unwind_Find_exidx", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t pcount = cpu->Regs()[1];

            if (pcount != 0) {
                memory.Write32(pcount, 0);
            }
            cpu->Regs()[0] = 0; // Null pointer
        });

        ROUTE_REGISTER(router, "__stack_chk_fail", [](Dynarmic::A32::Jit* cpu) {
            throw std::runtime_error("Stack Smashing Detected!");
        });

    }
}

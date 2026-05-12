#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "HostAssetManager.hpp"
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sys/types.h>
#include <zlib.h>
#include <unordered_map>
#include <cwchar>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <glad/gles2.h>

#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#define NETWORK_ENABLED

#ifdef NETWORK_ENABLED
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/socket.h>
#endif

struct EmulatedThread {
    uint32_t entry_point;
    uint32_t arg;
};

struct EmuThread {
    std::array<uint32_t, 16> regs;
    uint32_t cpsr;
    bool is_alive = true;
};

inline std::vector<EmulatedThread> pending_threads;

class AndroidEnvironment {
public:
        
    // Pass everything by reference so the lambdas can capture what they need
    static void RegisterAll(SyscallRouter& router, GuestMemory& memory, std::vector<EmuThread>& worker_threads) {

        // --- Virtual File System (VFS) ---
        static std::unordered_map<uint32_t, FILE*> open_files;
        static uint32_t next_file_handle = 0x1000; // Start fake handles at 0x1000

        
        auto translate_path = [](std::string path) -> std::string {
            // Reroute the fake Android data path to your local ./data folder
            std::string fake_prefix = "/fake/data/terraria";
            if (path.find(fake_prefix) == 0) {
                return "./data" + path.substr(fake_prefix.length());
            }
            return path; // ./obb and ./assets should already resolve correctly on the host
        };

        // --- Emulator Control ---
        router.Register("Emulator_Return_Trap", [](Dynarmic::A32::Jit* cpu) {
            // When the game hits this SVC, tell Dynarmic to pause execution and return to main.cpp!
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined1); 
        });
        
        // --- Threading / Misc ---
        router.Register("prctl", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0;
        });

        // --- Asset Manager ---
        router.Register("AAssetManager_fromJava", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = HostAssetManager::HANDLE_ID;
        });

        router.Register("AAssetManager_open", [&memory](Dynarmic::A32::Jit* cpu) {
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
            std::cout << "[AssetManager] Opening: " << full_path << std::endl;

            auto& asset = host_assets.open_files[host_assets.next_fd];
            asset.file.open(full_path, std::ios::binary | std::ios::ate);

            if (!asset.file.is_open()) {
                // --- Fallback for missing -rollover variants ---
                std::string fallback = filename;
                size_t pos = fallback.find("-rollover");
                if (pos != std::string::npos) {
                    fallback.erase(pos, 9);                 // strip "-rollover"
                    std::string fallback_path = host_assets.base_path + fallback;

                    asset.file.close();                     // clean up failed stream
                    asset.file.open(fallback_path, std::ios::binary | std::ios::ate);

                    if (asset.file.is_open()) {
                        std::cout << "[AssetManager] Fallback: " << filename
                                  << " -> " << fallback << std::endl;
                        full_path = fallback_path;
                    }
                }
                // ------------------------------------------------

                if (!asset.file.is_open()) {
                    std::cout << "[AssetManager] FAILED to open: " << full_path << std::endl;
                    host_assets.open_files.erase(host_assets.next_fd);
                    cpu->Regs()[0] = 0;
                    return;
                }
            }

            // CRITICAL: these must run for BOTH normal and fallback paths
            asset.length = asset.file.tellg();
            asset.file.seekg(0, std::ios::beg);
            cpu->Regs()[0] = host_assets.next_fd++;
        });

        /*
        router.Register("AAssetManager_open", [&memory](Dynarmic::A32::Jit* cpu) {
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
            std::cout << "[AssetManager] Opening: " << full_path << std::endl;

            auto& asset = host_assets.open_files[host_assets.next_fd];
            asset.file.open(full_path, std::ios::binary | std::ios::ate);

            if (!asset.file.is_open()) {
                std::cout << "[AssetManager] FAILED to open: " << full_path << std::endl;
                host_assets.open_files.erase(host_assets.next_fd);
                cpu->Regs()[0] = 0; 
                return;
            }

            asset.length = asset.file.tellg();
            asset.file.seekg(0, std::ios::beg);

            cpu->Regs()[0] = host_assets.next_fd++; 
        });*/

        router.Register("AAsset_read", [&memory](Dynarmic::A32::Jit* cpu) {
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

        router.Register("AAsset_close", [](Dynarmic::A32::Jit* cpu) {
            /*
            uint32_t asset_ptr = cpu->Regs()[0];
            host_assets.open_files.erase(asset_ptr);
            */
            // The game uses this to tell Android to free the asset buffer.
            // For now, doing absolutely nothing is the safest route to prevent 
            // use-after-free bugs while the engine is still initializing.
        });

        router.Register("AAsset_getLength", [](Dynarmic::A32::Jit* cpu) {
            uint32_t asset_ptr = cpu->Regs()[0];
            if (host_assets.open_files.find(asset_ptr) != host_assets.open_files.end()) {
                cpu->Regs()[0] = host_assets.open_files[asset_ptr].length;
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        router.Register("AAsset_getBuffer", [&memory](Dynarmic::A32::Jit* cpu) {
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

                // --- NEW DEBUGGING ---
                std::cout << "[AssetManager] -> Allocated " << asset.length << " bytes for guest buffer." << std::endl;
                if (asset.length >= 4) {
                    uint8_t* magic = memory.GetHostPointer(asset.buffer_ptr);
                    printf("[AssetManager] -> Magic Bytes: %02X %02X %02X %02X\n", magic[0], magic[1], magic[2], magic[3]);
                }
                // ---------------------
            }
            
            cpu->Regs()[0] = asset.buffer_ptr; // Return the guest memory pointer!
        });

        router.Register("malloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t size = cpu->Regs()[0];
            uint32_t ptr = memory.AllocateHeap(size);
            
            // It's helpful to log large allocations to track memory leaks later
            if (size > 1024 * 1024) { 
                std::cout << "[Bionic] Large malloc: " << size << " bytes at 0x" << std::hex << ptr << std::dec << std::endl;
            }

            std::cout << "-------^ malloc ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;
            
            cpu->Regs()[0] = ptr; 
        });

        router.Register("calloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nmemb = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t total = nmemb * size;
            if (total == 0) total = 1; // A real calloc returns a unique allocable pointer
    
            uint32_t ptr = memory.AllocateHeap(total);
            std::memset(memory.GetHostPointer(ptr), 0, total);
            cpu->Regs()[0] = ptr;
        });

        // Technically free does nothing in a bump allocator, but we need to intercept 
        // it so the game doesn't crash jumping to a null thunk.
        router.Register("free", [](Dynarmic::A32::Jit* cpu) {
            // Do nothing
        });

        // --- Android Logging ---
        router.Register("__android_log_print", [&memory](Dynarmic::A32::Jit* cpu) {
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

            // 64-bit fetch for doubles (Automatically skips odd registers for 8-byte alignment!)
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
                            // NEW: Handle pointer hex addresses!
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
            
            std::cout << "[Android Log] " << tag << ": " << output << std::endl;
            cpu->Regs()[0] = 0;
        });
        
        /*router.Register("strlen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            uint32_t len = 0;

            std::cout << "-------^ strlen ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;
            
            while (memory.Read8(ptr + len) != '\0') {
                len++;
            }
            cpu->Regs()[0] = len;
        });*/

        router.Register("strlen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            bool is_xml = (0x55b9dbc0 != 0 && ptr >= 0x55b9dbc0 && ptr < (0x55b9dbc0 + 0x223b));
    
            uint32_t len = 0;
            // Safely compute length (limit to 1MB to avoid hangs)
            while (len < 1024*1024 && memory.Read8(ptr + len) != '\0') {
                len++;
            }
    
            if (is_xml) {
                uint32_t offset = ptr - 0x55b9dbc0;
                uint32_t remaining = (0x55b9dbc0 + 0x223b) - ptr;
                std::cout << "[strlen] XML buffer: ptr=0x" << std::hex << ptr
                          << " offset=" << std::dec << offset
                          << " len=" << len
                          << " (remaining=" << remaining << ")"
                          << (len > remaining ? " *** OVERRUN ***" : "")
                          << std::endl;
                // Dump first 16 bytes at ptr
                std::cout << "  Data: ";
                for (int i = 0; i < 16; i++) {
                    uint8_t b = memory.Read8(ptr + i);
                    if (b >= 32 && b <= 126) std::cout << (char)b;
                    else printf("[%02X]", b);
                }
                std::cout << std::endl;
            }
    
            cpu->Regs()[0] = len;
        });

        router.Register("memset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint8_t val = static_cast<uint8_t>(cpu->Regs()[1]);
            uint32_t count = cpu->Regs()[2];

            std::cout << "-------^ memset ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;

            for (uint32_t i = 0; i < count; i++) {
                memory.Write8(dest + i, val);
            }
            cpu->Regs()[0] = dest; // memset returns the original pointer
        });

        router.Register("strncmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t s1 = cpu->Regs()[0];
            uint32_t s2 = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];

            std::cout << "-------^ strncmp ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "-------------------------" << std::endl;

            if (cpu->Regs()[1] == 0x4052b230 && cpu->Regs()[2] == 0x1) {
                uint8_t byte = memory.Read8(cpu->Regs()[0]);
                printf("strncmp: comparing byte at 0x%08x = 0x%02x ('%c') with '<'\n",
                       cpu->Regs()[0], byte, isprint(byte) ? byte : '.');
                // Also dump a few surrounding bytes
                printf("Surrounding: ");
                for (int i = -4; i <= 4; i++) {
                    uint8_t b = memory.Read8(cpu->Regs()[0] + i);
                    printf("%02x ", b);
                }
                printf("\n");
            }

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

        // --- C-String Functions ---
        router.Register("strcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t s1 = cpu->Regs()[0];
            uint32_t s2 = cpu->Regs()[1];
            const char* str1 = reinterpret_cast<const char*>(memory.GetHostPointer(s1));
            const char* str2 = reinterpret_cast<const char*>(memory.GetHostPointer(s2));
            std::cout << "[Router] strcmp: comparing '" << (str1 ? str1 : "(null)") 
              << "' with '" << (str2 ? str2 : "(null)") << "'" << std::endl;
            cpu->Regs()[0] = std::strcmp(str1, str2);
        });

        router.Register("strtod", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t nptr        = cpu->Regs()[0];
            uint32_t endptr_ptr  = cpu->Regs()[1];

            // Safely map the guest string; if null, feed an empty string to strtod
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

        /*router.Register("memchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            
            void* res = std::memchr(memory.GetHostPointer(ptr), ch, count);
            if (res) {
                // We found the character in host memory. We must calculate the offset
                // and add it to the original guest pointer to return a valid guest address!
                uint32_t offset = static_cast<uint8_t*>(res) - memory.GetHostPointer(ptr);
                cpu->Regs()[0] = ptr + offset;
            } else {
                cpu->Regs()[0] = 0; // Not found
            }
        });*/

        router.Register("memchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
    
            bool is_xml = (0x55b9dbc0 != 0 && ptr >= 0x55b9dbc0 && ptr < (0x55b9dbc0 + 0x223b));
    
            // Search using host memory
            uint8_t* host_ptr = memory.GetHostPointer(ptr);
            void* res = std::memchr(host_ptr, ch, count);
    
            if (res) {
                uint32_t offset = static_cast<uint8_t*>(res) - host_ptr;
                uint32_t guest_addr = ptr + offset;
                if (is_xml) {
                    std::cout << "[memchr] XML: ptr=0x" << std::hex << ptr
                              << " ch=0x" << ch << " ('" << (char)ch << "')"
                              << " count=" << std::dec << count
                              << " found at offset " << offset
                              << " -> 0x" << std::hex << guest_addr << std::dec
                              << std::endl;
                }
                cpu->Regs()[0] = guest_addr;
            } else {
                if (is_xml) {
                    std::cout << "[memchr] XML: ptr=0x" << std::hex << ptr
                              << " ch=0x" << ch << " ('" << (char)ch << "')"
                              << " count=" << std::dec << count
                              << " NOT FOUND" << std::endl;
                }
                cpu->Regs()[0] = 0;
            }
        });

        auto pthread_success = [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // 0 = Success
        };

        router.Register("pthread_mutexattr_init", pthread_success);
        router.Register("pthread_mutexattr_settype", pthread_success);
        router.Register("pthread_mutexattr_destroy", pthread_success);
        router.Register("pthread_mutex_init", pthread_success);
        router.Register("pthread_mutex_lock", pthread_success);
        router.Register("pthread_mutex_unlock", pthread_success);
        router.Register("pthread_mutex_destroy", pthread_success);
        router.Register("pthread_cond_init", pthread_success);
        
        router.Register("pthread_attr_setschedparam", pthread_success);
        router.Register("pthread_attr_setstacksize", pthread_success);
        router.Register("pthread_attr_setdetachstate", pthread_success);

        router.Register("pthread_once", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t once_control_ptr = cpu->Regs()[0];
            // Mark the control variable as "completed" (usually 2 in Bionic libc)
            // This prevents the game from getting stuck in an infinite init loop
            if (once_control_ptr != 0) memory.Write32(once_control_ptr, 2);
            cpu->Regs()[0] = 0; // Success
        });

        /*
        router.Register("pthread_create", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // Pretend we successfully spawned the thread
        });
        */

        router.Register("pthread_create", [&memory, &worker_threads](Dynarmic::A32::Jit* cpu) {
            uint32_t thread_ptr = cpu->Regs()[0];
            uint32_t entry = cpu->Regs()[2];
            uint32_t arg = cpu->Regs()[3];
            
            EmuThread t;
            t.regs.fill(0);
            
            // Allocate an isolated 1MB stack for this background thread
            uint32_t stack_size = 1024 * 1024; 
            uint32_t sp = memory.AllocateHeap(stack_size) + stack_size;
            
            t.regs[13] = sp;
            t.regs[0] = arg;
            t.regs[14] = 0xFFFFFFFF; // Magic exit address
            t.regs[15] = entry & ~1;
            t.cpsr = (entry & 1) ? 0x30 : 0x10; // Thumb or ARM mode
            
            worker_threads.push_back(t);
            std::cout << "\n[Threading] Spawned background thread " << worker_threads.size() << " at 0x" << std::hex << entry << std::dec << std::endl;
            
            if (thread_ptr) memory.Write32(thread_ptr, worker_threads.size());
            cpu->Regs()[0] = 0;
        });

        router.Register("pthread_self", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 1; // Return a dummy thread ID
        });

        router.Register("pthread_key_create", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // Success
        });

        router.Register("pthread_attr_init", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t attr = cpu->Regs()[0];
            if (attr) {
                for (int i = 0; i < 24; i++) memory.Write8(attr + i, 0);
            }
            cpu->Regs()[0] = 0;
        });

        router.Register("pthread_cond_timedwait", [&memory](Dynarmic::A32::Jit* cpu) {
            /*
            uint32_t abstime_ptr = cpu->Regs()[2];

            int64_t sleep_us = 1000; // hard minimum: always yield at least 1 ms

            if (abstime_ptr) {
                int32_t tv_sec  = static_cast<int32_t>(memory.Read32(abstime_ptr));
                int32_t tv_nsec = static_cast<int32_t>(memory.Read32(abstime_ptr + 4));

                // Use gettimeofday here too so we are guaranteed to match the guest
                struct timeval now;
                ::gettimeofday(&now, nullptr);
                int64_t now_us    = static_cast<int64_t>(now.tv_sec) * 1000000LL + now.tv_usec;
                int64_t abstime_us = static_cast<int64_t>(tv_sec) * 1000000LL + (tv_nsec / 1000LL);

                int64_t diff = abstime_us - now_us;
                if (diff > sleep_us) sleep_us = diff;
                if (sleep_us > 50000) sleep_us = 50000; // cap at 50ms so we don't hang the emu
            }

            if (sleep_us > 0) {
                usleep(static_cast<useconds_t>(sleep_us));
            } */

            cpu->Regs()[0] = ETIMEDOUT; // 110
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        });

        /*
        router.Register("pthread_cond_timedwait", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t abstime_ptr = cpu->Regs()[2];

            if (abstime_ptr) {
                int32_t tv_sec  = static_cast<int32_t>(memory.Read32(abstime_ptr));
                int32_t tv_nsec = static_cast<int32_t>(memory.Read32(abstime_ptr + 4));

                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);

                int64_t diff_sec  = static_cast<int64_t>(tv_sec)  - now.tv_sec;
                int64_t diff_nsec = static_cast<int64_t>(tv_nsec) - now.tv_nsec;
                if (diff_nsec < 0) {
                    diff_sec--;
                    diff_nsec += 1000000000LL;
                }

                if (diff_sec > 0 || (diff_sec == 0 && diff_nsec > 0)) {
                    int64_t total_us = diff_sec * 1000000LL + diff_nsec / 1000LL;
                    if (total_us > 0) {
                        // Cap at 1 second so we don't block the emulator forever
                        // (most game frames target ~16 ms anyway)
                        if (total_us > 1000000) total_us = 1000000;
                        usleep(static_cast<useconds_t>(total_us));
                    }
                }
            } else {
                usleep(1000); // safety nap if called with null timespec
            }

            // ETIMEDOUT tells the game "the timeout expired, no one signaled you"
            cpu->Regs()[0] = ETIMEDOUT;
        });*/


        router.Register("nanosleep", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0;
            // Force Dynarmic to break out of its execution loop
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2); 
        });

        router.Register("memcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            std::cout << "-------^ memcpy ^-------" << std::endl;
            std::cout << "R0 (This): 0x" << std::hex << cpu->Regs()[0] << std::dec << std::endl;
            std::cout << "R1: 0x" << std::hex << cpu->Regs()[1] << std::dec << std::endl;
            std::cout << "R2: 0x" << std::hex << cpu->Regs()[2] << std::dec << std::endl;
            std::cout << "R3: 0x" << std::hex << cpu->Regs()[3] << std::dec << std::endl;
            std::cout << "SP: 0x" << std::hex << cpu->Regs()[13] << std::dec << std::endl;
            std::cout << "CPSR:0x" << std::hex << cpu->Cpsr() << std::dec << std::endl;
            std::cout << "LR (R14) : 0x" << std::hex << cpu->Regs()[14] << std::dec << std::endl;
            std::cout << "PC (R15) : 0x" << std::hex << cpu->Regs()[15] << std::dec << std::endl;
            std::cout << "------------------------" << std::endl;
            
            std::memcpy(memory.GetHostPointer(dest), memory.GetHostPointer(src), count);
            
            cpu->Regs()[0] = dest; // memcpy returns destination pointer
        });

        router.Register("memmove", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            
            std::memmove(memory.GetHostPointer(dest), memory.GetHostPointer(src), count);
            
            cpu->Regs()[0] = dest; // memmove returns the destination pointer
        });

        router.Register("gettimeofday", [&memory](Dynarmic::A32::Jit* cpu) {
            struct timeval tv;
            ::gettimeofday(&tv, nullptr);
            uint32_t tv_ptr = cpu->Regs()[0];
            if (tv_ptr) {
                memory.Write32(tv_ptr,     static_cast<uint32_t>(tv.tv_sec));
                memory.Write32(tv_ptr + 4, static_cast<uint32_t>(tv.tv_usec));
            }
            cpu->Regs()[0] = 0;
        });

        /*
        router.Register("gettimeofday", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t tv_ptr = cpu->Regs()[0]; // struct timeval *
            
            if (tv_ptr != 0) {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                uint32_t sec = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                uint32_t usec = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000000;
                
                // Write the seconds and microseconds into the guest struct
                memory.Write32(tv_ptr, sec);
                memory.Write32(tv_ptr + 4, usec);
            }
            
            cpu->Regs()[0] = 0; // 0 = Success
        });*/

        // --- Standard Memory / C-String ---
        router.Register("memcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr1 = cpu->Regs()[0];
            uint32_t ptr2 = cpu->Regs()[1];
            uint32_t num = cpu->Regs()[2];
            
            // Safely extract up to 16 bytes as printable ASCII
            std::string s1, s2;
            for(uint32_t i = 0; i < std::min(num, 16u); i++) {
                char c1 = memory.Read8(ptr1 + i);
                char c2 = memory.Read8(ptr2 + i);
                s1 += (c1 >= 32 && c1 <= 126) ? c1 : '.';
                s2 += (c2 >= 32 && c2 <= 126) ? c2 : '.';
            }
            std::cout << "[Router] memcmp (" << num << " bytes): '" << s1 << "' vs '" << s2 << "'" << std::endl;
            
            int result = std::memcmp(memory.GetHostPointer(ptr1), memory.GetHostPointer(ptr2), num);

            // --- THE BULLETPROOF EXTENSION BYPASS ---
            if (num == 4) {
                std::string s2(reinterpret_cast<const char*>(memory.GetHostPointer(ptr2)), 4);
                
                // If the engine asks "Is this a .png?", we scream "YES!"
                if (s2 == ".png") {
                    std::cout << "\n[Hack] Engine asked for .png. Forcing match to bypass locale corruption!" << std::endl;
                    result = 0; // 0 = Strings match perfectly
                }
            }

            cpu->Regs()[0] = result;
        });

        router.Register("realloc", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t old_ptr = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];

            if (size == 0) {
                cpu->Regs()[0] = 0; // Effectively a free()
                return;
            }
            if (old_ptr == 0) {
                cpu->Regs()[0] = memory.AllocateHeap(size); // Effectively a malloc()
                return;
            }

            // Bump Allocator realloc hack:
            // We don't track the size of the old allocation, so we blindly copy 'size' bytes.
            // This will safely over-read into other guest allocations, but won't crash the host.
            uint32_t new_ptr = memory.AllocateHeap(size);
            std::memcpy(memory.GetHostPointer(new_ptr), memory.GetHostPointer(old_ptr), size);
            cpu->Regs()[0] = new_ptr;
        });

        // --- C++ Exception Handling ---
        router.Register("__gnu_Unwind_Find_exidx", [&memory](Dynarmic::A32::Jit* cpu) {
            // R0 = return_address, R1 = int* nump
            uint32_t pcount = cpu->Regs()[1];

            // Tell the C++ unwinder that there are 0 exception tables available
            if (pcount != 0) {
                memory.Write32(pcount, 0);
            }
            cpu->Regs()[0] = 0; // Null pointer
        });

        // --- Standard I/O (The Crash Logs) ---
        router.Register("fputs", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            
            std::string str = reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr));
            std::cout << "[Guest stdout] " << str;
            cpu->Regs()[0] = 1; // Return >= 0 for success
        });

        // new fwrite (kimi)
        router.Register("fwrite", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr   = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t nmemb = cpu->Regs()[2];
            uint32_t handle= cpu->Regs()[3];

            if (open_files.find(handle) != open_files.end() && ptr) {
                void* host_ptr = memory.GetHostPointer(ptr);
                cpu->Regs()[0] = std::fwrite(host_ptr, size, nmemb, open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        /*
        router.Register("fwrite", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            uint32_t total_bytes = size * count;

            std::string str(reinterpret_cast<const char*>(memory.GetHostPointer(ptr)), total_bytes);
            std::cout << "[Guest stdout] " << str;
            cpu->Regs()[0] = count; // Return number of elements written
        }); */

        // POSIX stubs (bionic stdio falls back to these internally sometimes)
        router.Register("read", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd         = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf   = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            // Fake EOF for everything except stdin
            if (fd == 0) {
                cpu->Regs()[0] = 0;
            } else {
                // You don't have a POSIX fd table yet; just claim EOF
                cpu->Regs()[0] = 0;
            }
        });

        router.Register("write", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd         = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf   = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            if (fd == 1 || fd == 2) {
                // Swallow stdout/stderr binary spam; return success
                cpu->Regs()[0] = count;
            } else {
                // Unknown fd: pretend it worked
                cpu->Regs()[0] = count;
            }
        });

        // bionic's errno is a TLS function returning int*
        static uint32_t guest_errno_ptr = 0;
        router.Register("__errno", [&memory](Dynarmic::A32::Jit* cpu) {
            if (guest_errno_ptr == 0) {
                guest_errno_ptr = memory.AllocateHeap(4);
                memory.Write32(guest_errno_ptr, 0);
            }
            cpu->Regs()[0] = guest_errno_ptr;
        });

        // --- Process Termination ---
        router.Register("abort", [](Dynarmic::A32::Jit* cpu) {
            std::cerr << "\\n[Bionic] Game called abort()!" << std::endl;
            // Throwing a C++ exception will cleanly break out of your cpu.Step() loop
            // and trigger the crash dump in main.cpp so you can see the final registers.
            throw std::runtime_error("Guest intentionally aborted execution.");
        });

        router.Register("__cxa_atexit", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // Success
        });

        // --- Math Functions ---
        router.Register("sqrtf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::sqrt(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        router.Register("sqrt", [](Dynarmic::A32::Jit* cpu) {
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

        // --- Floating Point Math ---
        router.Register("sinf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::sin(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        router.Register("cosf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            
            float f_out = std::cos(f_in);
            
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        // --- Wide Character Support ---
        router.Register("wctob", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c < 128) ? c : -1; // -1 is EOF
        });

        router.Register("btowc", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c != (uint32_t)-1) ? c : -1; // WEOF
        });

        router.Register("wctype", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 1; // Return a valid property ID
        });

        // --- String Utilities ---
        router.Register("strrchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            int ch = cpu->Regs()[1];
            
            const char* str = reinterpret_cast<const char*>(memory.GetHostPointer(ptr));
            const char* res = std::strrchr(str, ch);
            
            if (res) cpu->Regs()[0] = ptr + (res - str);
            else cpu->Regs()[0] = 0;
        });

        // --- libpng Error Handling ---
        router.Register("setjmp", [](Dynarmic::A32::Jit* cpu) {
            // Returning 0 tells libpng "We are initializing, no errors yet"
            cpu->Regs()[0] = 0; 
        });

        router.Register("longjmp", [](Dynarmic::A32::Jit* cpu) {
            // If libpng hits a fatal error, it will call longjmp to bail out.
            throw std::runtime_error("Guest called longjmp! libpng encountered a fatal error.");
        });

        router.Register("towlower", [](Dynarmic::A32::Jit* cpu) {
            uint32_t c = cpu->Regs()[0];
            cpu->Regs()[0] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
        });

        router.Register("crc32", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t crc_in = cpu->Regs()[0];
            uint32_t buf_ptr = cpu->Regs()[1];
            uint32_t len = cpu->Regs()[2];

            // Route directly to your host's native zlib library!
            if (buf_ptr == 0) {
                cpu->Regs()[0] = crc32(crc_in, Z_NULL, 0);
            } else {
                cpu->Regs()[0] = crc32(crc_in, memory.GetHostPointer(buf_ptr), len);
            }
        });

        // --- Math Additions ---
        router.Register("floor", [](Dynarmic::A32::Jit* cpu) {
            uint64_t raw_in = static_cast<uint64_t>(cpu->Regs()[0]) | (static_cast<uint64_t>(cpu->Regs()[1]) << 32);
            double d_in;
            std::memcpy(&d_in, &raw_in, sizeof(double));
            
            double d_out = std::floor(d_in);
            
            uint64_t raw_out;
            std::memcpy(&raw_out, &d_out, sizeof(double));
            cpu->Regs()[0] = static_cast<uint32_t>(raw_out & 0xFFFFFFFF);
            cpu->Regs()[1] = static_cast<uint32_t>(raw_out >> 32);
        });

        router.Register("floorf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            float f_out = std::floor(f_in);
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        router.Register("ceilf", [](Dynarmic::A32::Jit* cpu) {
            uint32_t raw_in = cpu->Regs()[0];
            float f_in;
            std::memcpy(&f_in, &raw_in, sizeof(float));
            float f_out = std::ceil(f_in);
            uint32_t raw_out;
            std::memcpy(&raw_out, &f_out, sizeof(float));
            cpu->Regs()[0] = raw_out;
        });

        // --- Zlib Decompression HLE ---

        // --- Zlib Decompression HLE (Pointer Safe) ---
        // Store pointers to the heap so the memory addresses never shift!
        static std::unordered_map<uint32_t, z_stream*> host_zstreams;

        router.Register("inflateInit_", [](Dynarmic::A32::Jit* cpu) {
            uint32_t strm_ptr = cpu->Regs()[0];
            
            z_stream* strm = new z_stream{};
            int ret = inflateInit(strm); 
            
            host_zstreams[strm_ptr] = strm;
            cpu->Regs()[0] = ret; 
        });

        router.Register("inflate", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t strm_ptr = cpu->Regs()[0];
            int flush = cpu->Regs()[1];
            
            if (host_zstreams.find(strm_ptr) == host_zstreams.end()) {
                std::cout << "[zlib] ERROR: Unknown stream pointer!" << std::endl;
                cpu->Regs()[0] = -2; // Z_STREAM_ERROR
                return;
            }
            
            z_stream* strm = host_zstreams[strm_ptr];
            
            // Read Guest State
            uint32_t g_next_in  = memory.Read32(strm_ptr + 0);
            uint32_t g_avail_in = memory.Read32(strm_ptr + 4);
            uint32_t g_next_out = memory.Read32(strm_ptr + 12);
            uint32_t g_avail_out= memory.Read32(strm_ptr + 16);
            
            // Map to Host State
            strm->next_in  = g_next_in ? memory.GetHostPointer(g_next_in) : nullptr;
            strm->avail_in = g_avail_in;
            strm->next_out = g_next_out ? memory.GetHostPointer(g_next_out) : nullptr;
            strm->avail_out= g_avail_out;
            
            // Decompress!
            int ret = inflate(strm, flush);
            
            // Calculate consumption
            uint32_t consumed_in  = g_avail_in - strm->avail_in;
            uint32_t produced_out = g_avail_out - strm->avail_out;
            
            // Update Guest State
            memory.Write32(strm_ptr + 0, g_next_in + consumed_in);
            memory.Write32(strm_ptr + 4, strm->avail_in);
            memory.Write32(strm_ptr + 8, memory.Read32(strm_ptr + 8) + consumed_in); 
            
            memory.Write32(strm_ptr + 12, g_next_out + produced_out);
            memory.Write32(strm_ptr + 16, strm->avail_out);
            memory.Write32(strm_ptr + 20, memory.Read32(strm_ptr + 20) + produced_out);
            
            std::cout << "[zlib] inflate(flush=" << flush << ") | "
                      << "IN: " << g_avail_in << " (used " << consumed_in << ") | "
                      << "OUT: " << g_avail_out << " (made " << produced_out << ") | "
                      << "RET: " << ret << std::endl;
            
            cpu->Regs()[0] = ret;
        });

        router.Register("inflateEnd", [](Dynarmic::A32::Jit* cpu) {
            uint32_t strm_ptr = cpu->Regs()[0];
            if (host_zstreams.find(strm_ptr) != host_zstreams.end()) {
                z_stream* strm = host_zstreams[strm_ptr];
                inflateEnd(strm);
                delete strm;
                host_zstreams.erase(strm_ptr);
            }
            cpu->Regs()[0] = 0; // Z_OK
        });

        // --- Wide String Operations (<cwchar>) ---
        router.Register("wcslen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            const wchar_t* str = reinterpret_cast<const wchar_t*>(memory.GetHostPointer(str_ptr));
            cpu->Regs()[0] = std::wcslen(str);
        });

        router.Register("wmemcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];
            
            std::wmemcpy(
                reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(src)),
                n
            );
            cpu->Regs()[0] = dest; // Returns destination pointer
        });

        router.Register("wmemcmp", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr1 = cpu->Regs()[0];
            uint32_t ptr2 = cpu->Regs()[1];
            uint32_t n = cpu->Regs()[2];
            
            cpu->Regs()[0] = std::wmemcmp(
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(ptr1)),
                reinterpret_cast<const wchar_t*>(memory.GetHostPointer(ptr2)),
                n
            );
        });

        router.Register("wmemset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            wchar_t ch = static_cast<wchar_t>(cpu->Regs()[1]);
            uint32_t n = cpu->Regs()[2];
            
            std::wmemset(reinterpret_cast<wchar_t*>(memory.GetHostPointer(dest)), ch, n);
            cpu->Regs()[0] = dest; // Returns destination pointer
        });

        router.Register("wmemchr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            wchar_t ch = static_cast<wchar_t>(cpu->Regs()[1]);
            uint32_t n = cpu->Regs()[2];
            
            wchar_t* host_ptr = reinterpret_cast<wchar_t*>(memory.GetHostPointer(ptr));
            wchar_t* res = std::wmemchr(host_ptr, ch, n);
            
            if (res) {
                // We must translate the host memory address BACK into a 32-bit guest address!
                uint32_t byte_offset = reinterpret_cast<uint8_t*>(res) - reinterpret_cast<uint8_t*>(host_ptr);
                cpu->Regs()[0] = ptr + byte_offset;
            } else {
                cpu->Regs()[0] = 0; // NULL
            }
        });

        // new fopen (kimi)
        router.Register("fopen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            uint32_t mode_ptr = cpu->Regs()[1];

            const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));
            const char* mode = reinterpret_cast<const char*>(memory.GetHostPointer(mode_ptr));

            FILE* fp = std::fopen(path, mode);
            if (!fp) {
                cpu->Regs()[0] = 0; // NULL
                return;
            }

            uint32_t handle = next_file_handle++;
            open_files[handle] = fp;
            cpu->Regs()[0] = handle;
        });

        /*
        router.Register("fopen", [&memory, translate_path](Dynarmic::A32::Jit* cpu) {
            uint32_t filename_ptr = cpu->Regs()[0];
            uint32_t mode_ptr = cpu->Regs()[1];
            
            std::string filename = reinterpret_cast<const char*>(memory.GetHostPointer(filename_ptr));
            std::string mode = reinterpret_cast<const char*>(memory.GetHostPointer(mode_ptr));
            
            std::string host_path = translate_path(filename);
            
            FILE* f = std::fopen(host_path.c_str(), mode.c_str());
            if (f) {
                uint32_t handle = next_file_handle++;
                open_files[handle] = f;
                cpu->Regs()[0] = handle;
            } else {
                cpu->Regs()[0] = 0; // NULL
            }
        });*/

        // int scandir(const char *dirp, struct dirent ***namelist, ...);
        router.Register("scandir", [&memory, translate_path](Dynarmic::A32::Jit* cpu) {
            uint32_t dirp_ptr = cpu->Regs()[0];
            uint32_t namelist_ptr = cpu->Regs()[1];
            
            std::string guest_dir = reinterpret_cast<const char*>(memory.GetHostPointer(dirp_ptr));
            std::string host_dir = translate_path(guest_dir);
            
            std::vector<std::string> found_files;
            if (std::filesystem::exists(host_dir) && std::filesystem::is_directory(host_dir)) {
                for (const auto& entry : std::filesystem::directory_iterator(host_dir)) {
                    found_files.push_back(entry.path().filename().string());
                }
            }

            if (found_files.empty()) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Allocate an array of pointers in guest memory for the namelist
            uint32_t array_ptr = memory.AllocateHeap(found_files.size() * 4);
            memory.Write32(namelist_ptr, array_ptr);

            // Bionic (Android) dirent struct layout:
            // uint64 d_ino, int64 d_off, uint16 d_reclen, uint8 d_type, char d_name[256]
            for (size_t i = 0; i < found_files.size(); i++) {
                uint32_t dirent_ptr = memory.AllocateHeap(280); 
                memory.Write32(array_ptr + (i * 4), dirent_ptr);
                
                // We only really need to populate d_name (offset 19) for the game to read it
                std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(dirent_ptr + 19)), found_files[i].c_str());
            }

            cpu->Regs()[0] = found_files.size();
        });

        router.Register("close", [](Dynarmic::A32::Jit* cpu) {
            int fd = static_cast<int>(cpu->Regs()[0]);
            cpu->Regs()[0] = ::close(fd);
        });

        router.Register("printf", [&memory](Dynarmic::A32::Jit* cpu) {
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
                    uint32_t lo = next_u32(), hi = next_u32();
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
                    uint32_t lo = next_u32(), hi = next_u32();
                    uint64_t raw = (uint64_t)lo | ((uint64_t)hi << 32);
                    double d; std::memcpy(&d, &raw, sizeof(d));
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%f", d); out += buf;
                } else {
                    out += '%'; out += spec;
                }
            }

            std::cout << "[Guest printf] " << out << std::endl;
            cpu->Regs()[0] = static_cast<int>(out.size());
        });

        // new fclose (kimi)
        router.Register("fclose", [](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];
            auto it = open_files.find(handle);
            if (it != open_files.end()) {
                std::fclose(it->second);
                open_files.erase(it);
                cpu->Regs()[0] = 0;
            } else {
                cpu->Regs()[0] = EOF;
            }
        });

        /*
        router.Register("fclose", [](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];
            if (open_files.find(handle) != open_files.end()) {
                std::fclose(open_files[handle]);
                open_files.erase(handle);
                cpu->Regs()[0] = 0; // Success
            } else {
                cpu->Regs()[0] = -1; // EOF / Error
            }
        }); */

        router.Register("fflush", [](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];
            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::fflush(open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        // new fread (kimi)
        // CORRECT argument order: R0=buf, R1=size, R2=nmemb, R3=handle
        router.Register("fread", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr   = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t nmemb = cpu->Regs()[2];
            uint32_t handle= cpu->Regs()[3];

            if (open_files.find(handle) != open_files.end() && ptr) {
                void* host_ptr = memory.GetHostPointer(ptr);
                cpu->Regs()[0] = std::fread(host_ptr, size, nmemb, open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });
        
        /* size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
        router.Register("fread", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t nmemb = cpu->Regs()[2];
            uint32_t handle = cpu->Regs()[3];

            if (open_files.find(handle) != open_files.end()) {
                void* host_ptr = memory.GetHostPointer(ptr);
                size_t elements_read = std::fread(host_ptr, size, nmemb, open_files[handle]);
                cpu->Regs()[0] = elements_read;
            } else {
                cpu->Regs()[0] = 0; // Error or EOF
            }
        }); */

        // int fseek(FILE *stream, long int offset, int whence);
        router.Register("fseek", [](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];
            long int offset = static_cast<long int>(cpu->Regs()[1]);
            int whence = cpu->Regs()[2];

            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::fseek(open_files[handle], offset, whence);
            } else {
                cpu->Regs()[0] = -1; // Error
            }
        });

        // long int ftell(FILE *stream);
        router.Register("ftell", [](Dynarmic::A32::Jit* cpu) {
            uint32_t handle = cpu->Regs()[0];

            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::ftell(open_files[handle]);
            } else {
                cpu->Regs()[0] = -1; // Error
            }
        });

        // int clock_gettime(clockid_t clock_id, struct timespec *tp);
        router.Register("clock_gettime", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t tp_ptr = cpu->Regs()[1];
            
            if (tp_ptr) {
                // Grab the real time from your host PC
                auto now = std::chrono::system_clock::now().time_since_epoch();
                uint32_t sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
                uint32_t nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
                
                // Write it into the guest's timespec struct
                memory.Write32(tp_ptr, sec);
                memory.Write32(tp_ptr + 4, nsec);
            }
            cpu->Regs()[0] = 0; // Success
        });

        // int vsprintf(char *str, const char *format, va_list ap);
        router.Register("vsprintf", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            uint32_t fmt_ptr = cpu->Regs()[1];
            uint32_t ap = cpu->Regs()[2]; // va_list pointer

            std::string format = reinterpret_cast<const char*>(memory.GetHostPointer(fmt_ptr));
            char* out_str = reinterpret_cast<char*>(memory.GetHostPointer(str_ptr));
            
            std::string result = "";
            for (size_t i = 0; i < format.length(); i++) {
                if (format[i] == '%' && i + 1 < format.length()) {
                    i++;
                    if (format[i] == 'd' || format[i] == 'i') {
                        int val = memory.Read32(ap); ap += 4;
                        result += std::to_string(val);
                    } else if (format[i] == 's') {
                        uint32_t ptr = memory.Read32(ap); ap += 4;
                        if (ptr) result += reinterpret_cast<const char*>(memory.GetHostPointer(ptr));
                        else result += "(null)";
                    } else if (format[i] == 'f') {
                        // Floats are promoted to 8-byte doubles in varargs, and must be 8-byte aligned!
                        if (ap % 8 != 0) ap += 4; 
                        uint64_t val = memory.Read64(ap); ap += 8;
                        double d; std::memcpy(&d, &val, sizeof(double));
                        result += std::to_string(d);
                    } else if (format[i] == 'x' || format[i] == 'X') {
                        int val = memory.Read32(ap); ap += 4;
                        char buf[16]; snprintf(buf, sizeof(buf), format[i] == 'x' ? "%x" : "%X", val);
                        result += buf;
                    } else {
                        result += format[i]; // Unhandled tag, just print it raw
                    }
                } else {
                    result += format[i];
                }
            }
            
            std::strcpy(out_str, result.c_str());
            cpu->Regs()[0] = result.length();
        });

        // int atoi(const char *str);
        router.Register("atoi", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            if (str_ptr != 0) {
                std::string str = reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr));
                cpu->Regs()[0] = std::atoi(str.c_str());
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        // time_t time(time_t *arg);
        router.Register("time", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t arg_ptr = cpu->Regs()[0];
            time_t current_time = std::time(nullptr);
            
            // If the game passes a valid pointer, we also have to write the time into that memory address
            if (arg_ptr != 0) {
                memory.Write32(arg_ptr, static_cast<uint32_t>(current_time));
            }
            
            // It always returns the time in R0 as well
            cpu->Regs()[0] = static_cast<uint32_t>(current_time);
        });

        // void srand48(long int seedval);
        // Note: srand48 is POSIX standard. 
        router.Register("srand48", [](Dynarmic::A32::Jit* cpu) {
            long int seed = static_cast<long int>(cpu->Regs()[0]);
            srand48(seed);
        });

        // ---------------------------------------------------------------------
        // Network / Socket passthrough
        // ---------------------------------------------------------------------

        router.Register("socket", [](Dynarmic::A32::Jit* cpu) {
            int domain   = static_cast<int>(cpu->Regs()[0]);
            int type     = static_cast<int>(cpu->Regs()[1]);
            int protocol = static_cast<int>(cpu->Regs()[2]);
            int fd       = ::socket(domain, type, protocol);
            cpu->Regs()[0] = fd;
        });

        router.Register("bind", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd       = static_cast<int>(cpu->Regs()[0]);
            uint32_t addr_ptr = cpu->Regs()[1];
            uint32_t addrlen  = cpu->Regs()[2];
            if (!addr_ptr) { cpu->Regs()[0] = -1; return; }

            const void* addr = memory.GetHostPointer(addr_ptr);
            int ret = ::bind(sockfd, static_cast<const struct sockaddr*>(addr), addrlen);
            cpu->Regs()[0] = ret;
        });

        router.Register("getsockname", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd            = static_cast<int>(cpu->Regs()[0]);
            uint32_t addr_ptr      = cpu->Regs()[1];
            uint32_t addrlen_ptr   = cpu->Regs()[2];
            if (!addr_ptr || !addrlen_ptr) { cpu->Regs()[0] = -1; return; }

            void* addr = memory.GetHostPointer(addr_ptr);
            socklen_t len = memory.Read32(addrlen_ptr);

            int ret = ::getsockname(sockfd, static_cast<struct sockaddr*>(addr), &len);
            memory.Write32(addrlen_ptr, static_cast<uint32_t>(len));
            cpu->Regs()[0] = ret;
        });

        router.Register("setsockopt", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd    = static_cast<int>(cpu->Regs()[0]);
            int level     = static_cast<int>(cpu->Regs()[1]);
            int optname   = static_cast<int>(cpu->Regs()[2]);
            uint32_t optval_ptr = cpu->Regs()[3];
            uint32_t optlen     = cpu->Regs()[4];
            if (!optval_ptr) { cpu->Regs()[0] = -1; return; }

            const void* optval = memory.GetHostPointer(optval_ptr);
            int ret = ::setsockopt(sockfd, level, optname, optval, optlen);
            cpu->Regs()[0] = ret;
        });

        router.Register("sendto", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd          = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf_ptr     = cpu->Regs()[1];
            uint32_t len         = cpu->Regs()[2];
            int flags            = static_cast<int>(cpu->Regs()[3]);
            uint32_t dest_ptr    = cpu->Regs()[4];
            uint32_t addrlen     = cpu->Regs()[5];
            if (!buf_ptr || !dest_ptr) { cpu->Regs()[0] = -1; return; }

            const void* buf  = memory.GetHostPointer(buf_ptr);
            const void* dest = memory.GetHostPointer(dest_ptr);

            ssize_t ret = ::sendto(sockfd, buf, len, flags,
                                   static_cast<const struct sockaddr*>(dest), addrlen);
            cpu->Regs()[0] = static_cast<uint32_t>(ret);
        });

        router.Register("inet_addr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t cp_ptr = cpu->Regs()[0];
            if (!cp_ptr) { cpu->Regs()[0] = INADDR_NONE; return; }

            const char* cp = reinterpret_cast<const char*>(memory.GetHostPointer(cp_ptr));
            cpu->Regs()[0] = ::inet_addr(cp);
        });

        router.Register("gethostname", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t name_ptr = cpu->Regs()[0];
            uint32_t len      = cpu->Regs()[1];
            if (!name_ptr) { cpu->Regs()[0] = -1; return; }

            char* name = reinterpret_cast<char*>(memory.GetHostPointer(name_ptr));
            cpu->Regs()[0] = ::gethostname(name, len);
        });

        router.Register("gethostbyname", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t name_ptr = cpu->Regs()[0];
            if (!name_ptr) { cpu->Regs()[0] = 0; return; }

            const char* name = reinterpret_cast<const char*>(memory.GetHostPointer(name_ptr));
            struct hostent* he = ::gethostbyname(name);
            if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Simplified guest copy: only h_name, h_addrtype, h_length, and one IPv4 in h_addr_list.
            uint32_t name_len = strlen(he->h_name) + 1;
            uint32_t total    = 20 + name_len + 8 + 4; // hostent + name + addrlist[2] + addr
            uint32_t base     = memory.AllocateHeap(total);

            uint32_t name_guest      = base + 20;
            uint32_t addr_list_guest = name_guest + name_len;
            uint32_t addr_guest      = addr_list_guest + 8;

            memcpy(memory.GetHostPointer(name_guest), he->h_name, name_len);
            memory.Write32(addr_list_guest + 0, addr_guest);
            memory.Write32(addr_list_guest + 4, 0);
            memcpy(memory.GetHostPointer(addr_guest), he->h_addr_list[0], 4);

            memory.Write32(base + 0,  name_guest);      // h_name
            memory.Write32(base + 4,  0);               // h_aliases = NULL
            memory.Write32(base + 8,  he->h_addrtype);  // h_addrtype
            memory.Write32(base + 12, he->h_length);    // h_length
            memory.Write32(base + 16, addr_list_guest); // h_addr_list

            cpu->Regs()[0] = base;
        });

        // --- OpenGL ES 2.0 Stubs ---
        // These functions return 'void', so we don't need to manipulate R0.
        // We just register them so they don't spam the console.
        auto gl_void_stub = [](Dynarmic::A32::Jit* cpu) {
            // Do nothing
        };

        // --- Standard Integers and Enums ---
        router.Register("glClear", [](Dynarmic::A32::Jit* cpu) { glClear(cpu->Regs()[0]); });
        router.Register("glEnable", [](Dynarmic::A32::Jit* cpu) { glEnable(cpu->Regs()[0]); });
        router.Register("glDisable", [](Dynarmic::A32::Jit* cpu) { glDisable(cpu->Regs()[0]); });
        router.Register("glDepthMask", [](Dynarmic::A32::Jit* cpu) { glDepthMask(cpu->Regs()[0]); });
        router.Register("glUseProgram", [](Dynarmic::A32::Jit* cpu) { glUseProgram(cpu->Regs()[0]); });
        router.Register("glCompileShader", [](Dynarmic::A32::Jit* cpu) { glCompileShader(cpu->Regs()[0]); });
        router.Register("glLinkProgram", [](Dynarmic::A32::Jit* cpu) { glLinkProgram(cpu->Regs()[0]); });
        router.Register("glActiveTexture", [](Dynarmic::A32::Jit* cpu) { glActiveTexture(cpu->Regs()[0]); });
        router.Register("glBindTexture", [](Dynarmic::A32::Jit* cpu) { glBindTexture(cpu->Regs()[0], cpu->Regs()[1]); });
        router.Register("glBlendFunc", [](Dynarmic::A32::Jit* cpu) { glBlendFunc(cpu->Regs()[0], cpu->Regs()[1]); });
        router.Register("glAttachShader", [](Dynarmic::A32::Jit* cpu) { glAttachShader(cpu->Regs()[0], cpu->Regs()[1]); });
        router.Register("glUniform1i", [](Dynarmic::A32::Jit* cpu) { glUniform1i(cpu->Regs()[0], cpu->Regs()[1]); });
        router.Register("glTexParameteri", [](Dynarmic::A32::Jit* cpu) { glTexParameteri(cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[2]); });
        router.Register("glViewport", [](Dynarmic::A32::Jit* cpu) { glViewport(cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[2], cpu->Regs()[3]); });

        // --- The Native ID Generators ---
        // Delete your gl_fake_handle logic completely! Let your real GPU generate the IDs.
        router.Register("glCreateShader", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glCreateShader(cpu->Regs()[0]); });
        router.Register("glCreateProgram", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glCreateProgram(); });

        router.Register("glDetachShader", [](Dynarmic::A32::Jit* cpu) { 
            glDetachShader(cpu->Regs()[0], cpu->Regs()[1]); 
        });

        router.Register("glDeleteShader", [](Dynarmic::A32::Jit* cpu) { 
            glDeleteShader(cpu->Regs()[0]); 
        });

        router.Register("glGenTextures", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t ptr = cpu->Regs()[1];
            glGenTextures(n, reinterpret_cast<GLuint*>(memory.GetHostPointer(ptr)));
        });

        router.Register("glDeleteTextures", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t ptr = cpu->Regs()[1];
            glDeleteTextures(n, reinterpret_cast<const GLuint*>(memory.GetHostPointer(ptr)));
        });

        router.Register("glBufferData", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t target = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t data_ptr = cpu->Regs()[2];
            uint32_t usage = cpu->Regs()[3];
            
            const void* host_data = data_ptr ? memory.GetHostPointer(data_ptr) : nullptr;
            glBufferData(target, size, host_data, usage);
        });

        router.Register("glShaderSource", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t shader = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t string_array_ptr = cpu->Regs()[2];
            uint32_t length_array_ptr = cpu->Regs()[3];

            std::vector<const GLchar*> strings(count);
            for(uint32_t i = 0; i < count; i++) {
                uint32_t str_ptr = memory.Read32(string_array_ptr + (i * 4));
                strings[i] = reinterpret_cast<const GLchar*>(memory.GetHostPointer(str_ptr));
            }

            const GLint* lengths = length_array_ptr ? reinterpret_cast<const GLint*>(memory.GetHostPointer(length_array_ptr)) : nullptr;
            glShaderSource(shader, count, strings.data(), lengths);
        });

        router.Register("glTexImage2D", [&memory](Dynarmic::A32::Jit* cpu) {
            // Args 1-4 are in the registers
            uint32_t target = cpu->Regs()[0];
            uint32_t level = cpu->Regs()[1];
            uint32_t internalformat = cpu->Regs()[2];
            uint32_t width = cpu->Regs()[3];

            // Args 5-9 are on the Stack! (SP is R13)
            uint32_t sp = cpu->Regs()[13];
            uint32_t height = memory.Read32(sp);
            uint32_t border = memory.Read32(sp + 4);
            uint32_t format = memory.Read32(sp + 8);
            uint32_t type = memory.Read32(sp + 12);
            uint32_t pixels_ptr = memory.Read32(sp + 16);

            const void* host_pixels = pixels_ptr ? memory.GetHostPointer(pixels_ptr) : nullptr;

            // Push all 9 to your desktop GPU!
            glTexImage2D(target, level, internalformat, width, height, border, format, type, host_pixels);
        });

        // --- 1. Float Bitcasting (The Clear Functions) ---
        // ARM32 passes floats in the raw integer registers. We must bitcast them back to C++ floats!
        router.Register("glClearColor", [](Dynarmic::A32::Jit* cpu) {
            float r, g, b, a;
            std::memcpy(&r, &cpu->Regs()[0], 4);
            std::memcpy(&g, &cpu->Regs()[1], 4);
            std::memcpy(&b, &cpu->Regs()[2], 4);
            std::memcpy(&a, &cpu->Regs()[3], 4);
            glClearColor(r, g, b, a);
        });

        router.Register("glClearDepthf", [](Dynarmic::A32::Jit* cpu) {
            float depth;
            std::memcpy(&depth, &cpu->Regs()[0], 4);
            glClearDepthf(depth); 
        });

        // --- 2. Standard Pass-Throughs ---
        router.Register("glBindBuffer", [](Dynarmic::A32::Jit* cpu) { 
            glBindBuffer(cpu->Regs()[0], cpu->Regs()[1]); 
        });

        router.Register("glIsProgram", [](Dynarmic::A32::Jit* cpu) { 
            cpu->Regs()[0] = glIsProgram(cpu->Regs()[0]); 
        });

        router.Register("glIsTexture", [](Dynarmic::A32::Jit* cpu) { 
            cpu->Regs()[0] = glIsTexture(cpu->Regs()[0]); 
        });

        // --- 3. Pointer Translations ---
        // Arrays and returned values must be mapped to your host's RAM.
        router.Register("glGenBuffers", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t buffers_ptr = cpu->Regs()[1];
            GLuint* host_buffers = reinterpret_cast<GLuint*>(memory.GetHostPointer(buffers_ptr));
            glGenBuffers(n, host_buffers);
        });

        router.Register("glGetIntegerv", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t pname = cpu->Regs()[0];
            uint32_t params_ptr = cpu->Regs()[1];
            GLint* host_params = reinterpret_cast<GLint*>(memory.GetHostPointer(params_ptr));
            
            // Your real desktop GPU will now answer things like "What is the max texture size?"
            glGetIntegerv(pname, host_params);
        });

        router.Register("glUniformMatrix4fv", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t location = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t transpose = cpu->Regs()[2];
            uint32_t value_ptr = cpu->Regs()[3];
            
            const GLfloat* host_value = reinterpret_cast<const GLfloat*>(memory.GetHostPointer(value_ptr));
            glUniformMatrix4fv(location, count, transpose, host_value);
        });

        // --- 4. String Translations ---
        router.Register("glGetUniformLocation", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t program = cpu->Regs()[0];
            uint32_t name_ptr = cpu->Regs()[1];
            const GLchar* name = reinterpret_cast<const GLchar*>(memory.GetHostPointer(name_ptr));
            
            cpu->Regs()[0] = glGetUniformLocation(program, name);
        });

        router.Register("glGetAttribLocation", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t program = cpu->Regs()[0];
            uint32_t name_ptr = cpu->Regs()[1];
            const GLchar* name = reinterpret_cast<const GLchar*>(memory.GetHostPointer(name_ptr));
            
            cpu->Regs()[0] = glGetAttribLocation(program, name);
        });


        // --- 5. The VBO Offset Trap (glDrawElements) ---
        router.Register("glDrawElements", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t mode = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t type = cpu->Regs()[2];
            uint32_t indices_val = cpu->Regs()[3];

            // TRICKY: If a game uses a Vertex Buffer Object (VBO), 'indices_val' is NOT a pointer. 
            // It is just an integer byte offset (e.g., 0, 12, 24).
            // But if it DOESN'T use a VBO, it's a real guest memory pointer that we have to translate!
            // We can safely guess: If the value is huge (like an 0x40000000 RAM address), it's a pointer.
            // If it's small, it's a VBO offset.
            
            const void* host_indices;
            if (indices_val > 0x100000) {
                // It's a raw pointer to guest RAM
                host_indices = memory.GetHostPointer(indices_val);
            } else {
                // It's a VBO offset. OpenGL expects us to cast the integer directly to a void pointer!
                host_indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_val));
            }

            glDrawElements(mode, count, type, host_indices);
        });
    }
};

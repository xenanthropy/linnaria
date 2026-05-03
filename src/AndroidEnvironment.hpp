#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "HostAssetManager.hpp"
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <zlib.h>
#include <unordered_map>

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
                std::cout << "[AssetManager] FAILED to open: " << full_path << std::endl;
                host_assets.open_files.erase(host_assets.next_fd);
                cpu->Regs()[0] = 0; 
                return;
            }

            asset.length = asset.file.tellg();
            asset.file.seekg(0, std::ios::beg);

            cpu->Regs()[0] = host_assets.next_fd++; 
        });

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

        router.Register("strlen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            uint32_t len = 0;
            while (memory.Read8(ptr + len) != '\0') {
                len++;
            }
            cpu->Regs()[0] = len;
        });

        router.Register("memset", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint8_t val = static_cast<uint8_t>(cpu->Regs()[1]);
            uint32_t count = cpu->Regs()[2];

            for (uint32_t i = 0; i < count; i++) {
                memory.Write8(dest + i, val);
            }
            cpu->Regs()[0] = dest; // memset returns the original pointer
        });

        router.Register("strncmp", [&memory](Dynarmic::A32::Jit* cpu) {
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

        router.Register("memchr", [&memory](Dynarmic::A32::Jit* cpu) {
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

        router.Register("nanosleep", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0;
            // Force Dynarmic to break out of its execution loop
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2); 
        });

        router.Register("memcpy", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dest = cpu->Regs()[0];
            uint32_t src = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            
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
        });

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

        router.Register("fwrite", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];
            uint32_t total_bytes = size * count;

            std::string str(reinterpret_cast<const char*>(memory.GetHostPointer(ptr)), total_bytes);
            std::cout << "[Guest stdout] " << str;
            cpu->Regs()[0] = count; // Return number of elements written
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

        // --- OpenGL ES 2.0 Stubs ---
        // These functions return 'void', so we don't need to manipulate R0.
        // We just register them so they don't spam the console.
        auto gl_void_stub = [](Dynarmic::A32::Jit* cpu) {
            // Do nothing
        };

        router.Register("glClearColor", gl_void_stub);
        router.Register("glClearDepthf", gl_void_stub);
        router.Register("glClear", gl_void_stub);
        router.Register("glViewport", gl_void_stub);
        router.Register("glAttachShader", gl_void_stub);
        router.Register("glLinkProgram", gl_void_stub);
        router.Register("glUseProgram", gl_void_stub);
        router.Register("glShaderSource", gl_void_stub);
        router.Register("glCompileShader", gl_void_stub);
        router.Register("glBindBuffer", gl_void_stub);
        router.Register("glBufferData", gl_void_stub);
        router.Register("glEnable", gl_void_stub);
        router.Register("glDisable", gl_void_stub);
        router.Register("glBlendFunc", gl_void_stub);
        router.Register("glDepthMask", gl_void_stub);
        router.Register("glUniform1i", gl_void_stub);
        router.Register("glUniformMatrix4fv", gl_void_stub);
        router.Register("glActiveTexture", gl_void_stub);
        router.Register("glDrawElements", gl_void_stub);
        router.Register("glDeleteTextures", gl_void_stub);
        router.Register("glBindTexture", gl_void_stub);
        router.Register("glTexParameteri", gl_void_stub);

        // glTexImage2D is the function that actually moves the pixels to VRAM.
        // It takes 9 arguments, but since we are stubbing it, we don't need to parse them.
        router.Register("glTexImage2D", gl_void_stub);

        // --- Active OpenGL Stubs ---
        // Some GL functions expect an integer ID back (like a shader handle). 
        // If we return 0, the game thinks OpenGL failed. We must return a fake ID.
        static uint32_t gl_fake_handle = 1;
        auto gl_return_handle = [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = gl_fake_handle++;
        };
        auto gl_return_true = [](Dynarmic::A32::Jit* cpu) { 
            cpu->Regs()[0] = 1; // GL_TRUE
        };

        router.Register("glCreateShader", gl_return_handle);
        router.Register("glCreateProgram", gl_return_handle);

        // --- Shader Locators (Safe Array Index Returns) ---
        router.Register("glGetUniformLocation", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; 
        });
        
        router.Register("glGetAttribLocation", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; 
        });

        router.Register("glIsProgram", gl_return_true);
        router.Register("glIsTexture", gl_return_true);
        router.Register("glGenBuffers", gl_return_true);

        router.Register("glGenTextures", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t textures_ptr = cpu->Regs()[1];
            
            // Write our fake texture IDs into the game's memory
            for (uint32_t i = 0; i < n; i++) {
                memory.Write32(textures_ptr + (i * 4), gl_fake_handle++);
            }
        });

        router.Register("glGetIntegerv", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t pname = cpu->Regs()[0];
            uint32_t params_ptr = cpu->Regs()[1];
            
            // 0x0D33 is GL_MAX_TEXTURE_SIZE
            if (pname == 0x0D33) {
                memory.Write32(params_ptr, 2048); // Tell the game we support up to 2048x2048 textures
            } else {
                memory.Write32(params_ptr, 0); // Default fallback for other queries
            }
        });
                
        // You can keep adding sections down here for EGL, libc, input, etc.
    }
};

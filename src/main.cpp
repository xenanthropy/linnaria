#include <ios>
#include <iostream>
#include <string>
#include <exception>

#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "EmuCallbacks.hpp"
#include "JNIEmulator.hpp"
#include "AndroidCP15.hpp"
#include "AndroidEnvironment.hpp"
#include "ThreadingHelpers.hpp"
#include "CPUHelper.hpp"
#include "Watchpoint.hpp"

#include <dynarmic/interface/optimization_flags.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/A32/a32.h>

#include <SDL2/SDL.h>
#include <glad/gles2.h>

thread_local Dynarmic::A32::Jit* active_cpu = nullptr;
thread_local uint32_t active_thread_id = 0;

void ExecuteGameFunction(Dynarmic::A32::Jit& cpu, GuestMemory& memory, ElfLoader& loader, 
                         const std::string& func_name, const std::vector<uint32_t>& args) {
    
    uint32_t func_addr = loader.GetExport(func_name);
    if (func_addr == 0) {
        std::cerr << "[Boot] Could not find function: " << func_name << std::endl;
        return;
    }

    std::cout << "\n=============================================" << std::endl;
    std::cout << "[Boot] Executing " << func_name << "..." << std::endl;

    // 1. Reset the Stack Pointer
    uint32_t sp = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;
    
    // 2. Push extra arguments to stack (Args 5+)
    if (args.size() > 4) {
        uint32_t stack_args = args.size() - 4;
        sp -= (stack_args * 4);
        if (sp % 8 != 0) sp -= 4; // Align stack to 8 bytes

        for (size_t i = 4; i < args.size(); i++) {
            memory.Write32(sp + ((i - 4) * 4), args[i]);
        }
    }
    cpu.Regs()[13] = sp;

    // 3. Populate R0-R3
    for (size_t i = 0; i < std::min(args.size(), (size_t)4); i++) {
        cpu.Regs()[i] = args[i];
    }

    // THE FIX: Set LR to our custom SVC trap
    cpu.Regs()[14] = loader.GetThunk("Emulator_Return_Trap");

    // The CPSR controls the CPU state. Bit 5 (0x20) is the Thumb flag.
    uint32_t cpsr = cpu.Cpsr();
    if (func_addr & 1) {
        cpsr |= 0x20;  // Switch to Thumb mode
    } else {
        cpsr &= ~0x20; // Switch to ARM mode
    }
    cpu.SetCpsr(cpsr);

    // Strip the Thumb bit!
    cpu.Regs()[15] = func_addr & ~1;

    // Run until Dynarmic returns our UserDefined1 halt reason
    while (true) {
        auto halt_reason = cpu.Run();

        if (halt_reason == Dynarmic::HaltReason::UserDefined1) {
            cpu.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            break;
        }
        // If the main thread yields, clear it and keep going
        if (halt_reason == Dynarmic::HaltReason::UserDefined2) {
            cpu.ClearHalt(Dynarmic::HaltReason::UserDefined2);
        }

        if (halt_reason == Dynarmic::HaltReason::UserDefined3) {
            cpu.ClearHalt(Dynarmic::HaltReason::UserDefined3);
            if (once_saved_state.valid) {
                for (int r = 0; r < 16; r++)
                    cpu.Regs()[r] = once_saved_state.regs[r];
                cpu.SetCpsr(once_saved_state.cpsr);
                once_saved_state.valid = false;
            }
        }
    }
}

void ExecuteGameFunction(Dynarmic::A32::Jit& cpu, GuestMemory& memory, ElfLoader& loader, 
                         uint32_t func_addr, const std::vector<uint32_t>& args) {
    if (func_addr == 0) return;

    std::cout << "\n=============================================" << std::endl;
    std::cout << "[Boot] Executing Thread at 0x" << std::hex << func_addr << std::dec << "..." << std::endl;

    uint32_t sp = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;
    
    if (args.size() > 4) {
        uint32_t stack_args = args.size() - 4;
        sp -= (stack_args * 4);
        if (sp % 8 != 0) sp -= 4; 
        for (size_t i = 4; i < args.size(); i++) {
            memory.Write32(sp + ((i - 4) * 4), args[i]);
        }
    }
    cpu.Regs()[13] = sp;

    for (size_t i = 0; i < std::min(args.size(), (size_t)4); i++) {
        cpu.Regs()[i] = args[i];
    }

    cpu.Regs()[14] = loader.GetThunk("Emulator_Return_Trap"); 
    
    uint32_t cpsr = cpu.Cpsr();
    if (func_addr & 1) cpsr |= 0x20;  
    else cpsr &= ~0x20; 
    cpu.SetCpsr(cpsr);

    cpu.Regs()[15] = func_addr & ~1; 

    while (true) {
        auto halt_reason = cpu.Run();
        if (halt_reason == Dynarmic::HaltReason::UserDefined1) {
            std::cout << "[Boot] Thread returned successfully!" << std::endl;
            cpu.ClearHalt(Dynarmic::HaltReason::UserDefined1);
            break;
        }
        if (halt_reason == Dynarmic::HaltReason::UserDefined3) {
            cpu.ClearHalt(Dynarmic::HaltReason::UserDefined3);
            if (once_saved_state.valid) {
                for (int r = 0; r < 16; r++)
                    cpu.Regs()[r] = once_saved_state.regs[r];
                cpu.SetCpsr(once_saved_state.cpsr);
                once_saved_state.valid = false;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;

    try {

        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        // Initialize SDL2 Video
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::cerr << "Failed to init SDL: " << SDL_GetError() << std::endl;
            return 1;
        }

        // Set OpenGL ES 2.0 Profile (Matches Android)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

        // Create the Window
        SDL_Window* window = SDL_CreateWindow(
            "Linnaria", 
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
            1280, 720, 
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
        );

        // Create the OpenGL Context so our host GPU can receive commands
        SDL_GLContext gl_context = SDL_GL_CreateContext(window);

        int glad_result = gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress);
        if (!glad_result) {
            printf("Failed to initialize GLAD\n");
            exit(1);
        }

        glClearColor(0.0f, 1.0f, 0.4f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(window);

        // Allocate the 8MB page table on the heap, safely.
        auto memory_ptr = std::make_unique<GuestMemory>();
        GuestMemory& memory = *memory_ptr;
        
        ElfLoader loader(memory);
        if (!loader.Load(argv[1])) return 1;

        // Initialize our clean Syscall Router
        SyscallRouter router;

        // --- Setup Global Monitor BEFORE AndroidEnvironment setup ---
        Dynarmic::ExclusiveMonitor monitor(256); // Support up to 256 hardware threads!
        
        // --- Register environment calls ---
        AndroidEnvironment::RegisterAll(router, memory, loader, monitor);

        // save all registered functions to file
        router.DumpSyscallMap("syscalls.txt");

        // 3. Configure Dynarmic CPU
        EmuCallbacks callbacks(memory, loader, router);
        Dynarmic::A32::UserConfig config;
        config.callbacks = &callbacks;

        // Give Dynarmic BOTH memory paths to prevent Xbyak fallback crashes
        config.page_table = &memory.page_table;
        config.absolute_offset_page_table = false;

        /* DEBUG: disable fastmem_pointer to check memory issues - leave on otherwise */
        //config.fastmem_pointer = reinterpret_cast<uintptr_t>(memory.fastmem_base);
        config.fastmem_pointer = 0;

        config.recompile_on_fastmem_failure = true;
        config.arch_version = Dynarmic::A32::ArchVersion::v7;

        config.global_monitor = &monitor;
        config.processor_id = 0; // Main Thread is Core 0
        active_thread_id = 0;
        uint32_t main_tls = memory.AllocateHeap(4096);
        config.coprocessors[15] = std::make_shared<AndroidCP15>(main_tls);

        // DEBUG: enable BIG BOI debugging for bad issues
        // Keep disabled otherwise, way too much logging
        //config.very_verbose_debugging_output = true;
        
        Dynarmic::A32::Jit cpu(config);
        callbacks.cpu = &cpu;
        // assign active cpu to CPUhelper
        active_cpu = &cpu;

        // Register the main thread's guest stack range. The main JIT's initial SP
        // is GuestMemory::CODE_BASE + MEMORY_SIZE - 0x100000 (0x7FFF0000) and grows
        // down. We conservatively claim the top 2 MiB. Owner is captured as the
        // current (main) host thread id.
        constexpr uint32_t MAIN_STACK_TOP  = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE;
        constexpr uint32_t MAIN_STACK_SIZE = 2 * 1024 * 1024;
        Watchpoint::Add(MAIN_STACK_TOP - MAIN_STACK_SIZE, MAIN_STACK_TOP, "main_stack");

        // Run C++ global constructors (init_array) before anything else
        for (uint32_t ctor : loader.GetConstructors()) {
            std::cout << "[Boot] Running static constructor at 0x" 
                      << std::hex << ctor << std::dec << std::endl;
            ExecuteGameFunction(cpu, memory, loader, ctor, {});
        }

        // 4. Find the actual game entry point
        uint32_t on_create = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnCreateActivity");
        if (!on_create) on_create = loader.GetExport("nativeOnCreateActivity");

        if (!on_create) {
            std::cerr << "[Fatal] Could not find nativeOnCreateActivity in the ELF!" << std::endl;
            return 1;
        }

        std::cout << "[Boot] Found nativeOnCreateActivity at 0x" << std::hex << on_create << std::dec << std::endl;

        // Setup initial execution state
        cpu.SetCpsr(0x00000030); // Thumb mode, User mode

        // Setup Stack Pointer
        cpu.Regs()[13] = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;
        uint32_t sp = cpu.Regs()[13];

        uint32_t env_ptr = JNIEmulator::Install(memory, loader, router);

        // 1. Create fake strings in Guest Memory matching the real Java call
        uint32_t dataPath = memory.AllocateHeap(64);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(dataPath)), "./data");

        uint32_t packageName = memory.AllocateHeap(64);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(packageName)), "com/codeglue/terraria");

        uint32_t language = memory.AllocateHeap(8);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(language)), "eng"); // ISO3 language code

        // 2. Emulate Android onCreate
        // Java Signature: nativeOnCreateActivity(AssetManager, int sdk, String dataPath, String package, String lang)
        // C++ Signature: (JNIEnv*, jclass, jobject, jint, jstring, jstring, jstring)
        ExecuteGameFunction(cpu, memory, loader, 
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnCreateActivity", 
            { env_ptr, 0, HostAssetManager::HANDLE_ID, 25, dataPath, packageName, language }
        );

        // 2. Emulate GLSurfaceView created
        ExecuteGameFunction(cpu, memory, loader, 
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnSurfaceChanged", 
            { env_ptr, 0 }
        );

        // 3. Emulate GLSurfaceView layout sizing (1280x720 screen)
        // signature: (JNIEnv*, jclass, jint w, jint h, jfloat cmW, jfloat cmH, jfloat diag)
        // Floats must be passed as raw 32-bit bitcasts
        uint32_t cmW, cmH, diag;
        float f_cmW = 14.0f, f_cmH = 7.0f, f_diag = 6.0f;
        
        // DEBUG: test bigger resolution when necessary (1920x1080 screen)
        //float f_cmW = 12.0f, f_cmH = 6.0f, f_diag = 14.0f;
        std::memcpy(&cmW, &f_cmW, 4);
        std::memcpy(&cmH, &f_cmH, 4);
        std::memcpy(&diag, &f_diag, 4);

        ExecuteGameFunction(cpu, memory, loader, 
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnResizeSurface", 
            { env_ptr, 0, 1280, 720, cmW, cmH, diag }
            // DEBUG: test bigger resolution when necessary (1920x1080 screen)
            //{ env_ptr, 0, 1920, 1080, cmW, cmH, diag }
        );

        // IGNORE: was experimenting with nativeOnExpansionFileExtracted and nativeUnlockGame
        uint32_t fake_path_ptr = memory.AllocateHeap(64);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(fake_path_ptr)), "./obb");
        uint32_t on_expansion_extracted = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnExpansionFileExtracted");
        if (on_expansion_extracted) {
            ExecuteGameFunction(cpu, memory, loader, "Java_com_codeglue_terraria_OctarineBridge_nativeOnExpansionFileExtracted", { env_ptr, 0, fake_path_ptr });
        }


        // Tell the engine we own the full game (Terraria.java calls this with true, "")
        uint32_t on_unlock = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeUnlockGame");
        if (on_unlock) {
            uint32_t empty_str_ptr = memory.AllocateHeap(4);
            std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(empty_str_ptr)), ""); 
            // env_ptr, clazz, jboolean (1 = true), jstring (0)
            ExecuteGameFunction(cpu, memory, loader, "Java_com_codeglue_terraria_OctarineBridge_nativeUnlockGame", { env_ptr, 0, 1, empty_str_ptr });
        }

        uint32_t resume_addr = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnResume");
        if (resume_addr) {
            ExecuteGameFunction(cpu, memory, loader, "Java_com_codeglue_terraria_OctarineBridge_nativeOnResume", { env_ptr, 0 });
        }

        std::cout << "\n=============================================" << std::endl;
        std::cout << "[Boot] Starting Main Game Loop!" << std::endl;

        bool running = true;
        uint32_t frame_count = 0;

        EmuThread main_thread;
        main_thread.is_alive = true;
        main_thread.regs.fill(0);
        main_thread.cpsr = 0x00000030; // Thumb, User mode
        main_thread.regs[13] = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;
        main_thread.regs[14] = loader.GetThunk("Emulator_Return_Trap");
        main_thread.regs[15] = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnUpdate") & ~1;

        // First-frame arguments
        // Java Signature: nativeOnUpdate(int i, int i2)
        // C++ Signature: (JNIEnv*, jclass, jint, jint)
        main_thread.regs[0] = env_ptr;
        main_thread.regs[1] = 0;
        main_thread.regs[2] = 1;
        main_thread.regs[3] = 1;
        ///////////////////////////////////

        while (running) {
            uint32_t frame_start = SDL_GetTicks();

            // --- 1. Host events ---
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) running = false;
            }

            // --- 2. Run Main Thread Slice ---
            if (main_thread.is_alive) {
                // Restore
                for (int r = 0; r < 16; r++) cpu.Regs()[r] = main_thread.regs[r];
                cpu.SetCpsr(main_thread.cpsr);

                auto halt = cpu.Run();

                // Save
                for (int r = 0; r < 16; r++) main_thread.regs[r] = cpu.Regs()[r];
                main_thread.cpsr = cpu.Cpsr();

                if (halt == Dynarmic::HaltReason::UserDefined1) {
                    cpu.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    // Function returned, this means one guest frame finished.
                    // For the next host frame, re-enter nativeOnUpdate.
                    main_thread.regs[15] = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnUpdate") & ~1;
                    main_thread.regs[0]  = env_ptr;
                    main_thread.regs[1]  = 0;
                    main_thread.regs[2]  = 1;
                    main_thread.regs[3]  = 1;
                    // Re-arm the return trap (just in case)
                    main_thread.regs[14] = loader.GetThunk("Emulator_Return_Trap");
                    main_thread.regs[13] = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;

                    // The return thunk is ARM code, so the saved CPSR is ARM mode.
                    // We must force Thumb mode again because nativeOnUpdate is a Thumb function.
                    main_thread.cpsr = 0x00000030;
                }
                else if (halt == Dynarmic::HaltReason::UserDefined2) {
                    cpu.ClearHalt(Dynarmic::HaltReason::UserDefined2);
                    // Just yield briefly if the main thread asks to sleep
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                else if (halt == Dynarmic::HaltReason::UserDefined3) {
                    cpu.ClearHalt(Dynarmic::HaltReason::UserDefined3);
                    // Restore the saved state from pthread_once redirection
                    if (once_saved_state.valid) {
                        for (int r = 0; r < 16; r++)
                            cpu.Regs()[r] = once_saved_state.regs[r];
                        cpu.SetCpsr(once_saved_state.cpsr);
                        once_saved_state.valid = false;
                        // The CPU now continues from the instruction right after the SVC that triggered pthread_once.
                        for (int r = 0; r < 16; r++)
                            main_thread.regs[r] = cpu.Regs()[r];
                        main_thread.cpsr = cpu.Cpsr();
                    }
                }
            }

            // --- 4. Present Frame ---
            SDL_GL_SwapWindow(window);

            /* DEBUG: messing with SDL delay rates - not very useful
            //--- 5. Host frame limiting ---
            uint32_t frame_time = SDL_GetTicks() - frame_start;
            if (frame_time < 16) {
                SDL_Delay(16 - frame_time);
            }
            */
        }

        // Clean up when the loop ends
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();

    } catch (const std::exception& e) {
        std::cerr << "\n[Fatal Exception] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#include <cmath>
#include <cstdint>
#include <ios>
#include <iostream>
#include <mutex>
#include <string>
#include <exception>
#include <cstdio>

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
#include "Pacing.hpp"
#include "Config.hpp"
#include "Input.hpp"
#include "dynarmic/interface/halt_reason.h"

#include <dynarmic/interface/optimization_flags.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/A32/a32.h>

#include <SDL2/SDL.h>
#include <glad/gles2.h>

thread_local Dynarmic::A32::Jit* active_cpu = nullptr;
thread_local uint32_t active_thread_id = 0;

void PatchGame(GuestMemory& memory) {
        // Thumb mode: movs r0, #0; bx lr;
        // Writes 0x2000 to the first halfword, and 0x4770 to the second.
        uint32_t thumb_ret_false = 0x47702000;

        if (Config::GameSettings::Patches::patchTouchscreenJoystick) {
            // 1. Make the joystick invisible
            // OnScreenJoystick::IsVisible
            memory.Write32(memory.CODE_BASE + 0x001424BC, thumb_ret_false);

            // 2. Prevent the UI from registering a touch so you can click the game world
            // OnScreenJoystick::IsInJoystickZone
            memory.Write32(memory.CODE_BASE + 0x00142470, thumb_ret_false);

            // 3. Stop the touch input manager from prioritizing the joystick
            // TouchInput::ShouldOnScreenJoystickBlockInput
            memory.Write32(memory.CODE_BASE + 0x003AE940, thumb_ret_false);
        }

        if (Config::GameSettings::Patches::patchControllerGuide) {
            // 4. Hide the Controller Guide (Bottom Button Bar) visually
            // ControllerGuideState::Draw
            memory.Write32(memory.CODE_BASE + 0x00103660, thumb_ret_false);

            // 5. Tell the engine logic that the Controller Guide is gone
            // ControllerGuideState::IsVisible
            memory.Write32(memory.CODE_BASE + 0x00103BAC, thumb_ret_false);
        }

        if (Config::GameSettings::Patches::patchMagnifyingWhileMining) {
            // Disable Magnifying Glass Visuals
            // 1. Disable Magnifying Glass border Rendering
            // UI::DrawMagnifyBorder
            memory.Write32(memory.CODE_BASE + 0x00331500, thumb_ret_false);

            // 2. Prevent Magnifier flag from setting during empty holds
            // UI::DrawMagnify (subroutine)
            memory.Write32(memory.CODE_BASE + 0x00338D88, 0x22004651);

            // 3. Kill Magnifier Background Rendering
            // Overwrites: BL WorldView::DrawBg
            memory.Write32(memory.CODE_BASE + 0x00339230, 0xBF00BF00);

            // 4. Kill Magnifier Tile Rendering
            // Overwrites: BL WorldView::DrawWorld(int)
            memory.Write32(memory.CODE_BASE + 0x00339238, 0xBF00BF00);
        }

        if (Config::GameSettings::Patches::patchAutoJump) {
            // Disable Auto-Jump
            memory.Write32(memory.CODE_BASE + 0x00272FB8, thumb_ret_false); // Player::CheckAutoJump
        }

        if (Config::GameSettings::Patches::patchMineDelay) {
            // Eradicate 1.0s Mining/Building Delay (Forces immediate Continuous Use)
            // UI::OnMagnifyTimerTick
            // Overwrites: CMP R0, #0; BEQ loc_32E2B2
            // With:       CMP R0, #0; NOP
            memory.Write32(memory.CODE_BASE + 0x0032E312, 0xBF002800);
        }

        if (Config::GameSettings::Patches::patchControllerReticle) {
            // UI::DrawCursor
            memory.Write32(memory.CODE_BASE + 0x00335FD4, thumb_ret_false);
        }

        if (Config::GameSettings::Patches::patchAutoDoorOpening) {
            // WorldGen::OpenDoors
            memory.Write32(memory.CODE_BASE + 0x003697F0, thumb_ret_false);
        }

        // thunk Platform::QuitGame call to exit game properly
        memory.Write32(memory.CODE_BASE + 0x00406784, 0xBF00DFFF);
}

void ExecuteGameFunction(Dynarmic::A32::Jit& cpu, GuestMemory& memory, ElfLoader& loader,
                         const std::string& func_name, const std::vector<uint32_t>& args,
                         bool verbose) {

    uint32_t func_addr = loader.GetExport(func_name);
    if (func_addr == 0) {
        std::cerr << "[Boot] Could not find function: " << func_name << std::endl;
        return;
    }

    if (Config::Prints::miscPrints) {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n=============================================" << std::endl;
        std::cout << "[Boot] Executing " << func_name << "..." << std::endl;
    }

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

    if (Config::Prints::miscPrints) {
        std::lock_guard<std::mutex> lock(console_mutex);
        std::cout << "\n=============================================" << std::endl;
        std::cout << "[Boot] Executing Thread at 0x" << std::hex << func_addr << std::dec << "..." << std::endl;
    }

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
            if (Config::Prints::miscPrints) {
                {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[Boot] Thread returned successfully!" << std::endl;
                }
            }
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
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
            std::cerr << "Failed to init SDL: " << SDL_GetError() << std::endl;
            return 1;
        }

        // Set OpenGL ES 2.0 Profile (Matches Android)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetSwapInterval(0);

        int windowWidth = Config::GameSettings::Upscale::upscaling ?
                          Config::GameSettings::Upscale::upscaleWidth :
                          Config::GameSettings::Resolution::nativeX;

        int windowHeight = Config::GameSettings::Upscale::upscaling ?
                           Config::GameSettings::Upscale::upscaleHeight :
                           Config::GameSettings::Resolution::nativeY;

        // Create the Window
        SDL_Window* window = SDL_CreateWindow(
            "Linnaria", 
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
            windowWidth, windowHeight,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
        );

        // Create the OpenGL Context so our host GPU can receive commands
        SDL_GLContext gl_context = SDL_GL_CreateContext(window);

        // We own pacing in software (see main loop below); don't let SwapWindow block.
        SDL_GL_SetSwapInterval(0);

        int glad_result = gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress);
        if (!glad_result) {
            printf("Failed to initialize GLAD\n");
            exit(1);
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(window);

        // Allocate the 8MB page table on the heap, safely.
        auto memory_ptr = std::make_unique<GuestMemory>();
        GuestMemory& memory = *memory_ptr;
        
        ElfLoader loader(memory);
        if (!loader.Load(argv[1])) return 1;

        // patch game in memory to enable/disable features
        PatchGame(memory);

        // Initialize our clean Syscall Router
        SyscallRouter router;

        // --- Setup Global Monitor BEFORE AndroidEnvironment setup ---
        Dynarmic::ExclusiveMonitor monitor(256); // Support up to 256 hardware threads!

        // 3. Configure Dynarmic CPU
        EmuCallbacks callbacks(memory, loader, router);
        Dynarmic::A32::UserConfig config;
        config.callbacks = &callbacks;

        // --- Register environment calls ---
        AndroidEnvironment::RegisterAll(router, memory, loader, monitor, callbacks);

        // save all registered functions to file
        router.DumpSyscallMap("syscalls.txt");

        // Give Dynarmic BOTH memory paths to prevent Xbyak fallback crashes
        config.page_table = &memory.page_table;
        config.absolute_offset_page_table = false;

        // Fastmem and Dynarmic verbose-trace controlled from Config.hpp.
        // Off -> all memory accesses route through EmuCallbacks (OOB trap +
        // Watchpoint check fire). On -> Dynarmic emits direct pointer
        // arithmetic against the page table for a major perf win.
        config.fastmem_pointer = Config::Performance::fastmem
            ? reinterpret_cast<uintptr_t>(memory.fastmem_base) : 0;
        config.recompile_on_fastmem_failure = false;
        config.recompile_on_exclusive_fastmem_failure = false;
        config.enable_cycle_counting = false;
        config.wall_clock_cntpct = true;
        config.fastmem_exclusive_access = true;
        config.arch_version = Dynarmic::A32::ArchVersion::v5TE;

        config.unsafe_optimizations = true;
        config.optimizations = Dynarmic::all_safe_optimizations
                             | Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA
                             | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP
                             | Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN
                             | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue
                             | Dynarmic::OptimizationFlag::BlockLinking;

        config.code_cache_size = 1024 * 1024 * 1024;

        config.global_monitor = &monitor;
        config.processor_id = 0; // Main Thread is Core 0
        active_thread_id = 0;
        uint32_t main_tls = memory.AllocateHeap(4096);
        config.coprocessors[15] = std::make_shared<AndroidCP15>(main_tls);

        config.very_verbose_debugging_output = Config::Performance::verboseDynarmic;

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
            if (Config::Prints::miscPrints) {
                {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[Boot] Running static constructor at 0x" 
                              << std::hex << ctor << std::dec << std::endl;
                }
            }
            ExecuteGameFunction(cpu, memory, loader, ctor, {});
        }

        // 4. Find the actual game entry point
        uint32_t on_create = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnCreateActivity");
        if (!on_create) on_create = loader.GetExport("nativeOnCreateActivity");

        if (!on_create) {
            std::cerr << "[Fatal] Could not find nativeOnCreateActivity in the ELF!" << std::endl;
            return 1;
        }

        if (Config::Prints::miscPrints) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "[Boot] Found nativeOnCreateActivity at 0x" << std::hex << on_create << std::dec << std::endl;
        }

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
        float f_cmW = 14.0f, f_cmH = 7.0f;

        float gameDiag = Config::GameSettings::Resolution::nativeDiag;

        std::memcpy(&cmW, &f_cmW, 4);
        std::memcpy(&cmH, &f_cmH, 4);
        std::memcpy(&diag, &gameDiag, 4);

        uint32_t gameWidth = Config::GameSettings::Resolution::nativeX;
        uint32_t gameHeight = Config::GameSettings::Resolution::nativeY;

        ExecuteGameFunction(cpu, memory, loader,
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnResizeSurface",
            { env_ptr, 0, gameWidth, gameHeight, cmW, cmH, diag }
        );

        // Lie to AndroidInterface::CheckMemoryInfo so extended worlds unlock.
        // JNI sig: setMemoryInformation(JNIEnv*, jobject, jdouble mb, jdouble totalRamKb).
        // AAPCS soft-float: mb -> R2:R3 (even-aligned pair), totalRamKb -> [SP+0:+4].
        // We pretend the device has 4 GiB: mb = 4096.0, totalRamKb = 4194304.0.
        {
            double mb_val = 4096.0;
            double total_ram_kb_val = 4194304.0;
            uint64_t mb_bits, total_bits;
            std::memcpy(&mb_bits, &mb_val, 8);
            std::memcpy(&total_bits, &total_ram_kb_val, 8);
            uint32_t mb_lo    = static_cast<uint32_t>(mb_bits);
            uint32_t mb_hi    = static_cast<uint32_t>(mb_bits >> 32);
            uint32_t total_lo = static_cast<uint32_t>(total_bits);
            uint32_t total_hi = static_cast<uint32_t>(total_bits >> 32);

            ExecuteGameFunction(cpu, memory, loader,
                "Java_com_codeglue_terraria_OctarineBridge_setMemoryInformation",
                { env_ptr, 0, mb_lo, mb_hi, total_lo, total_hi }
            );
        }

        // Tells game where obb file was extracted to (in our case, our local ./obb)
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

        if (Config::Prints::miscPrints) {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\n=============================================" << std::endl;
            std::cout << "[Boot] Starting Main Game Loop!" << std::endl;
        }

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

        // Pacing rates live in Pacing.hpp so HLE modules can flip them in
        // response to guest events (e.g., the Octarine achievement-system log
        // line bumps game_tick_hz to 60 once boot is past asset extraction).
        // Boot defaults: tick uncapped (fast asset load), display ~60 Hz (steady).

        uint32_t last_tick = SDL_GetTicks();
        uint32_t last_swap = SDL_GetTicks();

        // FPS / tick-rate window title. Both numbers are interesting separately:
        // tick = how often nativeOnUpdate completes (game-logic frame rate);
        // render = how often we actually SDL_GL_SwapWindow. When tick lags
        // render, the game itself is slow; when render lags tick, GL/host is
        // the bottleneck.
        uint32_t fps_swap_counter = 0;
        uint64_t fps_last_ud1 = 0;
        uint32_t fps_last_report = SDL_GetTicks();
        const uint32_t fps_report_interval_ms = 500;

        // True when main_thread is between native calls (last halt was
        // UserDefined1). Used to gate Input::DrainPending so we don't
        // re-use the stack while a guest function is paused on it.
        // Starts false: even if Pacing::input_enabled were forced true
        // before any tick has run, this keeps us from dispatching into a
        // game that hasn't completed its first nativeOnUpdate yet.
        bool main_thread_clean = false;

        // UserDefined1 fires every time the main guest thread completes a
        // nativeOnUpdate. Heartbeat prints every 60th one (~1 per second
        // at 60 Hz). If this counter stops advancing, cpu.Run is not
        // returning -- main is in a guest function that won't halt.
        uint64_t ud1_counter = 0;
        uint64_t last_ud1_report = 0;

        // Background watchdog thread: samples the main guest PC every
        // ~1s. If the same PC shows up for several consecutive samples,
        // main is in a busy-wait loop at that address. Look it up in IDA
        // to identify the function. Reading cpu.Regs()[15] from a
        // different thread is technically a data race, but uint32_t
        // loads are atomic on x86_64 and we only need a sample.
        std::atomic<bool> watchdog_running{true};
        std::thread watchdog;
        if constexpr (Config::Prints::heartbeat) {
            watchdog = std::thread([&cpu, &memory, &watchdog_running, &main_thread_clean]() {
                uint32_t prev_pc = 0;
                int unchanged = 0;
                bool dumped = false;
                while (watchdog_running.load()) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (!watchdog_running.load()) break;
                    uint32_t pc = cpu.Regs()[15];
                    if (pc == prev_pc) ++unchanged; else { unchanged = 0; dumped = false; }
                    prev_pc = pc;
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[Watchdog] main PC=0x" << std::hex << pc << std::dec
                              << "  unchanged=" << unchanged
                              << "  clean=" << (main_thread_clean ? "y" : "n")
                              << "\n";
                    // When the PC is wedged, dump the register file + a slice
                    // of the stack once. LR (R14) is the most recent return
                    // address; the stacked words are the call chain. Look
                    // them up in IDA to find who jumped into garbage.
                    if (unchanged >= 3 && !dumped) {
                        dumped = true;
                        std::cout << "[Watchdog] STUCK -- register dump:\n";
                        for (int r = 0; r < 16; r++)
                            std::cout << "    R" << r << "=0x" << std::hex << cpu.Regs()[r] << std::dec << "\n";
                        std::cout << "    CPSR=0x" << std::hex << cpu.Cpsr() << std::dec << "\n";
                        uint32_t sp = cpu.Regs()[13];
                        std::cout << "[Watchdog] stack from SP=0x" << std::hex << sp << ":\n";
                        for (int i = 0; i < 24; i++) {
                            uint32_t a = sp + i * 4;
                            std::cout << "    [SP+0x" << std::hex << (i * 4) << "]=0x" << memory.Read32(a) << std::dec << "\n";
                        }
                    }
                }
            });
        }

        while (running) {
            // --- 1. Host events ---
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) { running = false; continue; }
                if (event.type == SDL_KEYDOWN) {
                    bool consumed = true;
                    switch (event.key.keysym.sym) {
                        case SDLK_F1: Pacing::game_tick_hz.store(0);  break;
                        case SDLK_F2: Pacing::game_tick_hz.store(60); break;
                        case SDLK_F3: Pacing::display_hz.store(0);    break;
                        case SDLK_F4: Pacing::display_hz.store(60);   break;
                        default: consumed = false; break;
                    }
                    if (consumed) continue;
                }
                Input::HandleSDLEvent(event);
            }

            // --- 1b. Drain queued input events into the guest. Only when
            //         the main thread is at a clean return point so we
            //         don't trample a paused frame on the shared stack. ---
            if (main_thread_clean) {
                Input::DrainPending(cpu, memory, loader, env_ptr);
            }

            uint32_t game_tick_hz = Pacing::game_tick_hz.load();
            uint32_t display_hz   = Pacing::display_hz.load();
            uint32_t now = SDL_GetTicks();
            bool tick_due = (game_tick_hz == 0) || (now - last_tick >= 1000u / game_tick_hz);
            bool swap_due = (display_hz   == 0) || (now - last_swap >= 1000u / display_hz);

            if (tick_due && main_thread.is_alive) {
                // Ship one gamepad snapshot per tick. Gated on main_thread_clean
                // because ExecuteGameFunction resets SP to the main stack base
                // and would trample a yielded nativeOnUpdate's live frame
                // otherwise (same invariant as DrainPending). Skipping a send
                // during a yield is fine -- the engine retains the last sent
                // Gamepad state and we'll catch up on the next clean tick.
                if (main_thread_clean) {
                    Input::SendGamepadUpdate(cpu, memory, loader, env_ptr);
                }

                // Lock out input dispatch while the thread is working or suspended
                main_thread_clean = false;

                // Restore context to the JIT
                for (int r = 0; r < 16; r++) cpu.Regs()[r] = main_thread.regs[r];
                cpu.SetCpsr(main_thread.cpsr);

                // Keep driving the JIT until the guest explicitly yields or finishes the frame
                while (true) {
                    auto halt = cpu.Run();

                    if (halt == Dynarmic::HaltReason::UserDefined1) {
                        cpu.ClearHalt(Dynarmic::HaltReason::UserDefined1);

                        // Function returned; one guest frame finished.
                        // Re-arm the context for the next host frame.
                        main_thread.regs[15] = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnUpdate") & ~1;
                        main_thread.regs[0]  = env_ptr;
                        main_thread.regs[1]  = 0;
                        main_thread.regs[2]  = 1;
                        main_thread.regs[3]  = 1;
                        main_thread.regs[14] = loader.GetThunk("Emulator_Return_Trap");
                        main_thread.regs[13] = GuestMemory::CODE_BASE + GuestMemory::MEMORY_SIZE - 0x100000;
                        main_thread.cpsr = 0x00000030; // Force Thumb mode

                        // Frame is clean, input can be dispatched next loop
                        main_thread_clean = true;

                        ++ud1_counter;
                        if constexpr (Config::Prints::heartbeat) {
                            if (ud1_counter - last_ud1_report >= 60) {
                                last_ud1_report = ud1_counter;
                                std::lock_guard<std::mutex> lock(console_mutex);
                                std::cout << "[Heartbeat] nativeOnUpdate completions=" << ud1_counter << "\n";
                            }
                        }
                        break; // Exit inner loop
                    }
                    else if (halt == Dynarmic::HaltReason::UserDefined3) {
                        cpu.ClearHalt(Dynarmic::HaltReason::UserDefined3);

                        // Restore the saved state from pthread_once redirection
                        if (once_saved_state.valid) {
                            for (int r = 0; r < 16; r++) cpu.Regs()[r] = once_saved_state.regs[r];
                            cpu.SetCpsr(once_saved_state.cpsr);
                            once_saved_state.valid = false;
                        }
                        // Do not break; immediately loop back and call cpu.Run() again
                    }
                    else {
                        // Tick budget exhaustion (halt == 0) or another internal halt
                        if (static_cast<uint64_t>(halt) != 0) {
                            cpu.ClearHalt(halt);
                        }
                        // Do not break; immediately loop back and call cpu.Run() again
                    }
                }

                last_tick = now;
            }

            // --- 3. Present (if swap budget is due AND back buffer has new
            //         content). Skipping the swap when frame_dirty is false
            //         avoids the ping-pong flicker that happens when a logic-
            //         only nativeOnUpdate didn't redraw the back buffer.
            bool swapped = false;
            if (swap_due && Pacing::frame_dirty.load(std::memory_order_relaxed)) {
                SDL_GL_SwapWindow(window);
                Pacing::frame_dirty.store(false, std::memory_order_relaxed);
                last_swap = now;
                swapped = true;
                ++fps_swap_counter;
            }

            // Update window title with measured rates every fps_report_interval_ms.
            if (now - fps_last_report >= fps_report_interval_ms) {
                uint32_t elapsed_ms = now - fps_last_report;
                uint32_t render_fps = (fps_swap_counter * 1000u) / elapsed_ms;
                uint32_t tick_fps   = static_cast<uint32_t>(((ud1_counter - fps_last_ud1) * 1000ULL) / elapsed_ms);
                char title[128];
                std::snprintf(title, sizeof(title),
                    "Linnaria  |  tick: %u fps  |  render: %u fps",
                    tick_fps, render_fps);
                SDL_SetWindowTitle(window, title);
                fps_swap_counter = 0;
                fps_last_ud1 = ud1_counter;
                fps_last_report = now;
            }

            // --- 4. Idle yield: don't pin a core when nothing's due ---
            if (!tick_due && !swapped) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }

        // Clean up when the loop ends
        watchdog_running.store(false);
        if (watchdog.joinable()) watchdog.join();
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();

        exit(0);

    } catch (const std::exception& e) {
        std::cerr << "\n[Fatal Exception] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

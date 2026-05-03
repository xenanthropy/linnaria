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

#include <dynarmic/interface/optimization_flags.h>
#include <dynarmic/interface/exclusive_monitor.h>

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

    // 5. Run until exit

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
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;

    try {
        // Allocate the 8MB page table on the heap, safely.
        auto memory_ptr = std::make_unique<GuestMemory>();
        GuestMemory& memory = *memory_ptr;
        
        ElfLoader loader(memory);
        if (!loader.Load(argv[1])) return 1;

        // 1. Initialize our clean Syscall Router
        SyscallRouter router;

        ////// NEWWWWW. 
        std::vector<EmuThread> worker_threads;
        AndroidEnvironment::RegisterAll(router, memory, worker_threads);

        // 3. Configure Dynarmic CPU
        EmuCallbacks callbacks(memory, loader, router);
        Dynarmic::A32::UserConfig config;
        config.callbacks = &callbacks;

        // Give Dynarmic BOTH memory paths to prevent Xbyak fallback crashes
        config.page_table = &memory.page_table;
        config.absolute_offset_page_table = false;
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(memory.fastmem_base);
        config.recompile_on_fastmem_failure = true;

        config.arch_version = Dynarmic::A32::ArchVersion::v7;

        Dynarmic::ExclusiveMonitor monitor(1); 
        config.global_monitor = &monitor;
        config.coprocessors[15] = std::make_shared<AndroidCP15>();

        // enable BIG BOI debugging
        //config.very_verbose_debugging_output = true;
        
        Dynarmic::A32::Jit cpu(config);
        callbacks.cpu = &cpu;

        // 4. Find the actual game entry point!
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
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(dataPath)), "/fake/data/terraria");

        uint32_t packageName = memory.AllocateHeap(64);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(packageName)), "com/codeglue/terraria");

        uint32_t language = memory.AllocateHeap(8);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(language)), "eng"); // ISO3 language code

        // 2. Emulate Android onCreate
        // Java Signature: nativeOnCreateActivity(AssetManager, int sdk, String dataPath, String package, String lang)
        // C++ Signature: (JNIEnv*, jclass, jobject, jint, jstring, jstring, jstring)
        ExecuteGameFunction(cpu, memory, loader, 
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnCreateActivity", 
            { env_ptr, 0, HostAssetManager::HANDLE_ID, 19, dataPath, packageName, language }
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
        std::memcpy(&cmW, &f_cmW, 4);
        std::memcpy(&cmH, &f_cmH, 4);
        std::memcpy(&diag, &f_diag, 4);

        ExecuteGameFunction(cpu, memory, loader, 
            "Java_com_codeglue_terraria_OctarineBridge_nativeOnResizeSurface", 
            { env_ptr, 0, 1280, 720, cmW, cmH, diag }
        );

        /* IGNORE: was experimenting with nativeOnExpansionFileExtracted and nativeUnlockGame
        uint32_t fake_path_ptr = memory.AllocateHeap(64);
        std::strcpy(reinterpret_cast<char*>(memory.GetHostPointer(fake_path_ptr)), "/data/data/com.codeglue.terraria/files/");
        uint32_t on_expansion_extracted = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnExpansionFileExtracted");
        if (on_expansion_extracted) {
            std::cout << "[Boot] Firing nativeOnExpansionFileExtracted..." << std::endl;
            ExecuteGameFunction(cpu, memory, loader, on_expansion_extracted, { env_ptr, 0, fake_path_ptr });
        }

        // 2. Tell the engine we own the full game (Terraria.java calls this with true, "")
        uint32_t on_unlock = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeUnlockGame");
        if (on_unlock) {
            std::cout << "[Boot] Firing nativeUnlockGame..." << std::endl;
            // env_ptr, clazz, jboolean (1 = true), jstring (0)
            ExecuteGameFunction(cpu, memory, loader, on_unlock, { env_ptr, 0, 1, 0 });
        }
        */

        // 4. The Main Engine Loop
        // Java Signature: nativeOnUpdate(int i, int i2)
        // C++ Signature: (JNIEnv*, jclass, jint, jint)
        
        std::cout << "\n=============================================" << std::endl;
        std::cout << "[Boot] Starting Main Game Loop!" << std::endl;

        uint32_t resume_addr = loader.GetExport("Java_com_codeglue_terraria_OctarineBridge_nativeOnResume");
        if (resume_addr) {
            std::cout << "[Boot] Firing nativeOnResume..." << std::endl;
            ExecuteGameFunction(cpu, memory, loader, resume_addr, { env_ptr, 0 });
        }

        // Testing 4 frames for now. 4th frame starts actual render loop
        for (int frame = 1; frame <= 4; frame++) {
            std::cout << "\n--- Rendering Frame " << frame << " ---" << std::endl;
            ExecuteGameFunction(cpu, memory, loader, 
                "Java_com_codeglue_terraria_OctarineBridge_nativeOnUpdate", 
                { env_ptr, 0, 1, 1 }
            );

            // 2. Time-slice the Background Workers
            for (size_t i = 0; i < worker_threads.size(); i++) {
                auto& thread = worker_threads[i];
                if (!thread.is_alive) continue;
                
                // Swap in the worker's registers
                for(int r = 0; r < 16; r++) cpu.Regs()[r] = thread.regs[r];
                cpu.SetCpsr(thread.cpsr);
                
                // Run the worker until it hits nanosleep or exits
                auto halt = cpu.Run();
                
                if (halt == Dynarmic::HaltReason::UserDefined2) {
                    cpu.ClearHalt(Dynarmic::HaltReason::UserDefined2);
                } else if (cpu.Regs()[15] == 0xFFFFFFFF || cpu.Regs()[15] == 0xFFFFFFFE) {
                    std::cout << "[Threading] Worker Thread " << (i+1) << " exited." << std::endl;
                    thread.is_alive = false;
                }
                
                // Save the worker's registers for the next frame
                for(int r = 0; r < 16; r++) thread.regs[r] = cpu.Regs()[r];
                thread.cpsr = cpu.Cpsr();
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "\n[Fatal Exception] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

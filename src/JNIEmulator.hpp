#pragma once
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "JNIFunctions.hpp"

class JNIEmulator {
public:
    static constexpr uint32_t JNI_BASE = GuestMemory::CODE_BASE + 0x8000000; // 128MB offset

    static uint32_t Install(GuestMemory& mem, ElfLoader& loader, SyscallRouter& router) {
        uint32_t env_ptr = JNI_BASE;
        uint32_t table_ptr = JNI_BASE + 4;

        // --- NEW: The Global JavaVM ---
        uint32_t jvm_ptr = JNI_BASE + 0x1000; 
        uint32_t jvm_vtable = JNI_BASE + 0x1004;
        mem.Write32(jvm_ptr, jvm_vtable);

        // JavaVM Index 4: AttachCurrentThread
        router.Register("JavaVM_AttachCurrentThread", [env_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t penv = cpu->Regs()[1];
            mem.Write32(penv, env_ptr); 
            cpu->Regs()[0] = 0; // JNI_OK
        });
        mem.Write32(jvm_vtable + (4 * 4), loader.GetThunk("JavaVM_AttachCurrentThread"));

        // JavaVM Index 6: GetEnv
        router.Register("JavaVM_GetEnv", [env_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t penv = cpu->Regs()[1];
            mem.Write32(penv, env_ptr);
            cpu->Regs()[0] = 0; // JNI_OK
        });
        mem.Write32(jvm_vtable + (6 * 4), loader.GetThunk("JavaVM_GetEnv"));
        // ------------------------------

        mem.Write32(env_ptr, table_ptr);

        for (size_t i = 0; i < JNIFunctions::jni_function_names.size(); i++) {
            std::string name = "JNI_" + std::string(JNIFunctions::jni_function_names[i]);
            uint32_t thunk_addr = loader.GetThunk(name);
            mem.Write32(table_ptr + (i * 4), thunk_addr);
    
            router.Register(name, [i, name](Dynarmic::A32::Jit* cpu) {
                std::cout << "[JNI] WARNING: Unimplemented JNI function: " << name << std::endl;
                cpu->Regs()[0] = 0; 
            });
        }

        // Overwrite standard JNI Env indices
        mem.Write32(table_ptr + (6 * 4), loader.GetThunk("JNI_FindClass"));
        mem.Write32(table_ptr + (33 * 4), loader.GetThunk("JNI_GetMethodID"));
        mem.Write32(table_ptr + (169 * 4), loader.GetThunk("JNI_GetStringUTFChars"));
        mem.Write32(table_ptr + (170 * 4), loader.GetThunk("JNI_ReleaseStringUTFChars"));
        mem.Write32(table_ptr + (215 * 4), loader.GetThunk("JNI_RegisterNatives"));
        mem.Write32(table_ptr + (219 * 4), loader.GetThunk("JNI_GetJavaVM"));

        router.Register("JNI_FindClass", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x1337; });
        router.Register("JNI_GetMethodID", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x7331; });

        router.Register("JNI_GetStringUTFChars", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t jstr_ptr = cpu->Regs()[1];
            uint32_t isCopy_ptr = cpu->Regs()[2];
            if (isCopy_ptr != 0) mem.Write8(isCopy_ptr, 0); 
            cpu->Regs()[0] = jstr_ptr; 
        });

        router.Register("JNI_ReleaseStringUTFChars", [](Dynarmic::A32::Jit* cpu) {});

        router.Register("JNI_RegisterNatives", [&mem](Dynarmic::A32::Jit* cpu) {
            // ... (Your existing RegisterNatives logic remains exactly the same) ...
            cpu->Regs()[0] = 0; 
        });

        // The Fix! Hand the engine the real JavaVM pointer!
        router.Register("JNI_GetJavaVM", [jvm_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t vm_ptr_ptr = cpu->Regs()[1];
            mem.Write32(vm_ptr_ptr, jvm_ptr); 
            cpu->Regs()[0] = 0; 
        });

        router.Register("JNI_GetStaticMethodID", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0x7332; // Dummy non-zero method ID
        });
        router.Register("JNI_CallStaticVoidMethodV", [](Dynarmic::A32::Jit* cpu) {
            // No-op: the Java method doesn't exist on the host anyway
        });

        return env_ptr;
    }
};

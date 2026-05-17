#pragma once
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "JNIFunctions.hpp"
#include <mutex>

class JNIEmulator {
public:
    static constexpr uint32_t JNI_BASE = GuestMemory::CODE_BASE + 0x8000000; // 128MB offset

    static uint32_t Install(GuestMemory& mem, ElfLoader& loader, SyscallRouter& router) {
        uint32_t env_ptr = JNI_BASE;
        uint32_t table_ptr = JNI_BASE + 4;

        uint32_t jvm_ptr = JNI_BASE + 0x1000; 
        uint32_t jvm_vtable = JNI_BASE + 0x1004;
        mem.Write32(jvm_ptr, jvm_vtable);

        // JavaVM Index 4: AttachCurrentThread
        ROUTE_REGISTER(router, "JavaVM_AttachCurrentThread", [env_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t penv = cpu->Regs()[1];
            mem.Write32(penv, env_ptr); 
            cpu->Regs()[0] = 0; // JNI_OK
        });
        mem.Write32(jvm_vtable + (4 * 4), loader.GetThunk("JavaVM_AttachCurrentThread"));

        // JavaVM Index 6: GetEnv
        ROUTE_REGISTER(router, "JavaVM_GetEnv", [env_ptr, &mem](Dynarmic::A32::Jit* cpu) {
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

            ROUTE_REGISTER(router, name, [i, name](Dynarmic::A32::Jit* cpu) {
                {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[JNI] WARNING: Unimplemented JNI function: " << "[" << i << "] " << name << std::endl;
                }
                cpu->Regs()[0] = 0; 
            });
        }

        // Overwrite standard JNI Env indices
        mem.Write32(table_ptr + (6 * 4), loader.GetThunk("JNI_FindClass"));
        mem.Write32(table_ptr + (19 * 4), loader.GetThunk("JNI_PushLocalFrame"));
        mem.Write32(table_ptr + (21 * 4), loader.GetThunk("JNI_NewGlobalRef"));
        mem.Write32(table_ptr + (29 * 4), loader.GetThunk("JNI_NewObjectV"));
        mem.Write32(table_ptr + (33 * 4), loader.GetThunk("JNI_GetMethodID"));
        mem.Write32(table_ptr + (80 * 4), loader.GetThunk("JNI_CallNonvirtualIntMethodV"));
        mem.Write32(table_ptr + (92 * 4), loader.GetThunk("JNI_CallNonvirtualVoidMethodV"));
        mem.Write32(table_ptr + (130 * 4), loader.GetThunk("JNI_CallStaticIntMethodV"));
        mem.Write32(table_ptr + (169 * 4), loader.GetThunk("JNI_GetStringUTFChars"));
        mem.Write32(table_ptr + (170 * 4), loader.GetThunk("JNI_ReleaseStringUTFChars"));
        mem.Write32(table_ptr + (176 * 4), loader.GetThunk("JNI_NewByteArray"));
        mem.Write32(table_ptr + (215 * 4), loader.GetThunk("JNI_RegisterNatives"));
        mem.Write32(table_ptr + (219 * 4), loader.GetThunk("JNI_GetJavaVM"));
        mem.Write32(table_ptr + (222 * 4), loader.GetThunk("JNI_GetPrimitiveArrayCritical"));
        mem.Write32(table_ptr + (223 * 4), loader.GetThunk("JNI_ReleasePrimitiveArrayCritical"));

        ROUTE_REGISTER(router, "JNI_FindClass", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x1337; });
        ROUTE_REGISTER(router, "JNI_GetMethodID", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x7331; });

        ROUTE_REGISTER(router, "JNI_GetStringUTFChars", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t jstr_ptr = cpu->Regs()[1];
            uint32_t isCopy_ptr = cpu->Regs()[2];
            if (isCopy_ptr != 0) mem.Write8(isCopy_ptr, 0); 
            cpu->Regs()[0] = jstr_ptr; 
        });

        ROUTE_REGISTER(router, "JNI_ReleaseStringUTFChars", [](Dynarmic::A32::Jit* cpu) {});

        ROUTE_REGISTER(router, "JNI_RegisterNatives", [&mem](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; 
        });

        ROUTE_REGISTER(router, "JNI_GetJavaVM", [jvm_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t vm_ptr_ptr = cpu->Regs()[1];
            mem.Write32(vm_ptr_ptr, jvm_ptr); 
            cpu->Regs()[0] = 0; 
        });

        ROUTE_REGISTER(router, "JNI_GetStaticMethodID", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0x7332; // Dummy non-zero method ID
        });

        ROUTE_REGISTER(router, "JNI_CallStaticVoidMethodV", [](Dynarmic::A32::Jit* cpu) {
            // No-op: the Java method doesn't exist on the host anyway
        });

        ROUTE_REGISTER(router, "JNI_GetPrimitiveArrayCritical", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t array_ptr = cpu->Regs()[1];
            uint32_t isCopy_ptr = cpu->Regs()[2];
            if (isCopy_ptr) mem.Write8(isCopy_ptr, 0); // Tell Java it's not a copy, it's raw memory
            cpu->Regs()[0] = array_ptr; 
        });

        ROUTE_REGISTER(router, "JNI_ReleasePrimitiveArrayCritical",[](Dynarmic::A32::Jit* cpu) {
            // Do nothing. Memory stays where it is.
        });

        // JNI_NewByteArray: The game wants a Java array to store data. 
        // Hack: We can just AllocateHeap and hand back a raw memory pointer. The game will treat it like a JNI array
        ROUTE_REGISTER(router, "JNI_NewByteArray", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t length = cpu->Regs()[1]; 
            cpu->Regs()[0] = mem.AllocateHeap(length); 
        });

        ROUTE_REGISTER(router, "JNI_PushLocalFrame",[](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0; }); // JNI_OK
        ROUTE_REGISTER(router, "JNI_NewGlobalRef", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = cpu->Regs()[1]; }); // Return the same ref
        ROUTE_REGISTER(router, "JNI_NewObjectV", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x88888888; }); // Dummy Object
        ROUTE_REGISTER(router, "JNI_CallNonvirtualVoidMethodV",[](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0; });
        ROUTE_REGISTER(router, "JNI_CallNonvirtualIntMethodV",[](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0; });
        ROUTE_REGISTER(router, "JNI_CallStaticIntMethodV", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0; }); // Stub to 0

        return env_ptr;
    }
};

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

        // In C, a JNIEnv* is a pointer to a pointer to the function table
        mem.Write32(env_ptr, table_ptr);

        for (size_t i = 0; i < JNIFunctions::jni_function_names.size(); i++) {
            std::string name = "JNI_" + std::string(JNIFunctions::jni_function_names[i]);
            // Replace invalid characters for thunk names (e.g., "reserved0" becomes "JNI_reserved0")
            uint32_t thunk_addr = loader.GetThunk(name);
            mem.Write32(table_ptr + (i * 4), thunk_addr);
    
            router.Register(name, [i, name](Dynarmic::A32::Jit* cpu) {
                std::cout << "[JNI] WARNING: Unimplemented JNI function: " << name 
                          << " (index " << i << ")" << std::endl;
                cpu->Regs()[0] = 0; // Return null safely
            });
        }

        // Overwrite the specific indices the game actually uses
        mem.Write32(table_ptr + (6 * 4), loader.GetThunk("JNI_FindClass"));
        mem.Write32(table_ptr + (33 * 4), loader.GetThunk("JNI_GetMethodID"));
        mem.Write32(table_ptr + (169 * 4), loader.GetThunk("JNI_GetStringUTFChars"));
        mem.Write32(table_ptr + (170 * 4), loader.GetThunk("JNI_ReleaseStringUTFChars"));
        mem.Write32(table_ptr + (215 * 4), loader.GetThunk("JNI_RegisterNatives"));
        mem.Write32(table_ptr + (219 * 4), loader.GetThunk("JNI_GetJavaVM"));

        router.Register("JNI_FindClass", [](Dynarmic::A32::Jit* cpu) {
            std::cout << "[JNI] FindClass called!" << std::endl;
            cpu->Regs()[0] = 0x1337; // Hand the game a fake Class ID
        });

        router.Register("JNI_GetMethodID", [](Dynarmic::A32::Jit* cpu) {
            std::cout << "[JNI] GetMethodID called!" << std::endl;
            cpu->Regs()[0] = 0x7331; // Hand the game a fake Method ID
        });

        // Index 169: const char* GetStringUTFChars(JNIEnv *env, jstring string, jboolean *isCopy)
        router.Register("JNI_GetStringUTFChars", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t jstr_ptr = cpu->Regs()[1];
            uint32_t isCopy_ptr = cpu->Regs()[2];

            // Tell the game we did NOT make a copy of the string (JNI_FALSE = 0)
            if (isCopy_ptr != 0) {
                mem.Write8(isCopy_ptr, 0); 
            }
            
            // Return our raw memory pointer as the C-string
            cpu->Regs()[0] = jstr_ptr; 
        });

        // Index 170: void ReleaseStringUTFChars(JNIEnv *env, jstring string, const char *utf)
        router.Register("JNI_ReleaseStringUTFChars", [](Dynarmic::A32::Jit* cpu) {
            // We aren't actually copying strings, so there's nothing to free. Do nothing.
        });

        // Index 215: jint RegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint nMethods)
        router.Register("JNI_RegisterNatives", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t methods_ptr = cpu->Regs()[2];
            uint32_t num_methods = cpu->Regs()[3];

            std::cout << "\n--- [JNI] Game Registering Native Methods ---" << std::endl;
            
            for (uint32_t i = 0; i < num_methods; i++) {
                uint32_t struct_base = methods_ptr + (i * 12);
                uint32_t name_ptr = mem.Read32(struct_base + 0);
                uint32_t sig_ptr  = mem.Read32(struct_base + 4);
                uint32_t fn_ptr   = mem.Read32(struct_base + 8);

                std::string name;
                uint32_t offset = 0;
                char c;
                while ((c = mem.Read8(name_ptr + offset)) != '\0') {
                    name += c;
                    offset++;
                }

                std::cout << "Registered: " << name << " -> 0x" << std::hex << fn_ptr << std::dec << std::endl;
            }
            std::cout << "---------------------------------------------\n" << std::endl;

            cpu->Regs()[0] = 0; // 0 = JNI_OK
        });

        // Index 219: jint GetJavaVM(JNIEnv *env, JavaVM **vm)
        router.Register("JNI_GetJavaVM", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t vm_ptr_ptr = cpu->Regs()[1];
            
            // The game wants us to write a pointer to the JavaVM into this address.
            // We can just give it a fake memory address for now.
            mem.Write32(vm_ptr_ptr, GuestMemory::CODE_BASE + 0x4000000); 
            cpu->Regs()[0] = 0; // 0 = JNI_OK
        });

        return env_ptr;
    }
};

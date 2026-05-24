#pragma once
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "SyscallRouter.hpp"
#include "JNIFunctions.hpp"
#include "HLE/HLE_Audio.hpp"
#include <mutex>
#include <map>

class JNIEmulator {
public:
    static constexpr uint32_t JNI_BASE = GuestMemory::CODE_BASE + 0x8000000; // 128MB offset

    // Method-ID registry. GetMethodID/GetStaticMethodID returns a unique ID per
    // (name, signature) pair; the *Method* dispatch handlers recover the pair
    // and route to the right behavior (currently: AudioTrack methods). Names
    // can collide across classes ("write" exists in lots of Java classes), so
    // we key on name+signature, which is distinctive enough in practice.
    struct MethodInfo { std::string name; std::string sig; };
    static inline std::mutex method_mutex;
    static inline std::map<uint32_t, MethodInfo> method_registry;
    static inline uint32_t next_method_id = 0x10000;

    static uint32_t RegisterMethod(const std::string& name, const std::string& sig) {
        std::lock_guard<std::mutex> lock(method_mutex);
        for (auto& [id, info] : method_registry) {
            if (info.name == name && info.sig == sig) return id;
        }
        uint32_t id = next_method_id++;
        method_registry[id] = { name, sig };
        return id;
    }

    // Returns "<name><sig>" e.g. "write([BII)I". Empty if unknown.
    static std::string MethodKey(uint32_t id) {
        std::lock_guard<std::mutex> lock(method_mutex);
        auto it = method_registry.find(id);
        if (it == method_registry.end()) return "";
        return it->second.name + it->second.sig;
    }

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
        mem.Write32(table_ptr + (118 * 4), loader.GetThunk("JNI_CallStaticBooleanMethodV"));
        mem.Write32(table_ptr + (130 * 4), loader.GetThunk("JNI_CallStaticIntMethodV"));
        mem.Write32(table_ptr + (23 * 4),  loader.GetThunk("JNI_DeleteLocalRef"));
        mem.Write32(table_ptr + (167 * 4), loader.GetThunk("JNI_NewStringUTF"));
        mem.Write32(table_ptr + (169 * 4), loader.GetThunk("JNI_GetStringUTFChars"));
        mem.Write32(table_ptr + (170 * 4), loader.GetThunk("JNI_ReleaseStringUTFChars"));
        mem.Write32(table_ptr + (176 * 4), loader.GetThunk("JNI_NewByteArray"));
        mem.Write32(table_ptr + (215 * 4), loader.GetThunk("JNI_RegisterNatives"));
        mem.Write32(table_ptr + (219 * 4), loader.GetThunk("JNI_GetJavaVM"));
        mem.Write32(table_ptr + (222 * 4), loader.GetThunk("JNI_GetPrimitiveArrayCritical"));
        mem.Write32(table_ptr + (223 * 4), loader.GetThunk("JNI_ReleasePrimitiveArrayCritical"));

        ROUTE_REGISTER(router, "JNI_FindClass", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = 0x1337; });

        // Reads a NUL-terminated string from guest memory byte-by-byte.
        // Used by GetMethodID / GetStaticMethodID to capture the method name
        // and signature for later dispatch.
        auto read_guest_string = [&mem](uint32_t ptr) -> std::string {
            std::string s;
            if (!ptr) return s;
            uint32_t p = ptr;
            while (char c = static_cast<char>(mem.Read8(p++))) s += c;
            return s;
        };

        ROUTE_REGISTER(router, "JNI_GetMethodID", [&mem, read_guest_string](Dynarmic::A32::Jit* cpu) {
            std::string name = read_guest_string(cpu->Regs()[2]);
            std::string sig  = read_guest_string(cpu->Regs()[3]);
            cpu->Regs()[0] = RegisterMethod(name, sig);
        });

        ROUTE_REGISTER(router, "JNI_GetStringUTFChars", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t jstr_ptr = cpu->Regs()[1];
            uint32_t isCopy_ptr = cpu->Regs()[2];
            if (isCopy_ptr != 0) mem.Write8(isCopy_ptr, 0);
            cpu->Regs()[0] = jstr_ptr;
        });

        ROUTE_REGISTER(router, "JNI_ReleaseStringUTFChars", [](Dynarmic::A32::Jit* cpu) {});

        // NewStringUTF(env, const char* utf) -> jstring. We don't have real
        // Java string objects; the existing GetStringUTFChars handler just
        // returns the jstring pointer back as the char*, so we can do the
        // inverse here: hand back the input C string as the "jstring", and
        // any later GetStringUTFChars on it round-trips correctly.
        ROUTE_REGISTER(router, "JNI_NewStringUTF", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = cpu->Regs()[1];
        });

        // DeleteLocalRef(env, local_ref) -> void. We don't track JNI refs.
        ROUTE_REGISTER(router, "JNI_DeleteLocalRef", [](Dynarmic::A32::Jit* cpu) {});

        ROUTE_REGISTER(router, "JNI_RegisterNatives", [&mem](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; 
        });

        ROUTE_REGISTER(router, "JNI_GetJavaVM", [jvm_ptr, &mem](Dynarmic::A32::Jit* cpu) {
            uint32_t vm_ptr_ptr = cpu->Regs()[1];
            mem.Write32(vm_ptr_ptr, jvm_ptr); 
            cpu->Regs()[0] = 0; 
        });

        ROUTE_REGISTER(router, "JNI_GetStaticMethodID", [&mem, read_guest_string](Dynarmic::A32::Jit* cpu) {
            std::string name = read_guest_string(cpu->Regs()[2]);
            std::string sig  = read_guest_string(cpu->Regs()[3]);
            cpu->Regs()[0] = RegisterMethod(name, sig);
        });

        ROUTE_REGISTER(router, "JNI_CallStaticVoidMethodV", [](Dynarmic::A32::Jit* cpu) {
            // No-op: the Java method doesn't exist on the host anyway
        });

        // Boolean-returning static calls (e.g. OctarineBridge::GoogleSignedIn())
        // -- we have no Java to dispatch to, so always return false. Silences
        // the [JNI] unimplemented warning at slot 118.
        ROUTE_REGISTER(router, "JNI_CallStaticBooleanMethodV", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0;
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

        // NewObjectV(env, jclass, jmethodID, va_list) -- 4 fixed args, no
        // variadic at this ABI level (variadic was flattened by the C++
        // wrapper NewObject before it called us). So R0=env, R1=class,
        // R2=methodID, R3=va_list (a guest pointer into the wrapper's
        // arg-spill area; variadic args are packed at 4-byte boundaries).
        ROUTE_REGISTER(router, "JNI_NewObjectV", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t methodID = cpu->Regs()[2];
            std::string key = MethodKey(methodID);

            if (key == "<init>(IIIIII)V") {
                // AudioTrack(streamType, sampleRate, channelConfig,
                //            audioFormat, bufferSize, mode)
                uint32_t va = cpu->Regs()[3];
                uint32_t streamType    = mem.Read32(va +  0);
                uint32_t sampleRate    = mem.Read32(va +  4);
                uint32_t channelConfig = mem.Read32(va +  8);
                uint32_t audioFormat   = mem.Read32(va + 12);
                uint32_t bufferSize    = mem.Read32(va + 16);
                uint32_t mode          = mem.Read32(va + 20);
                (void)streamType; (void)bufferSize; (void)mode;

                int channels = (channelConfig == 3) ? 2 : 1;  // STEREO=3, MONO=2
                int bits     = (audioFormat == 2)   ? 16 : 8; // PCM_16BIT=2, PCM_8BIT=3
                HLE::Audio::OpenDevice(static_cast<int>(sampleRate), channels, bits);
            }

            cpu->Regs()[0] = 0x88888888; // dummy jobject; engine treats as opaque handle
        });

        ROUTE_REGISTER(router, "JNI_CallStaticIntMethodV", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t methodID = cpu->Regs()[2];
            std::string key = MethodKey(methodID);

            if (key == "getMinBufferSize(III)I") {
                cpu->Regs()[0] = HLE::Audio::GetMinBufferSize();
                return;
            }
            cpu->Regs()[0] = 0;
        });

        // CallNonvirtualVoidMethodV(env, obj, jclass, jmethodID, va_list).
        // R0=env, R1=obj, R2=class, R3=methodID, [SP+0]=va_list.
        ROUTE_REGISTER(router, "JNI_CallNonvirtualVoidMethodV", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t methodID = cpu->Regs()[3];
            std::string key = MethodKey(methodID);

            if      (key == "play()V")    HLE::Audio::Play();
            else if (key == "stop()V")    HLE::Audio::Stop();
            else if (key == "release()V") HLE::Audio::Close();
        });

        ROUTE_REGISTER(router, "JNI_CallNonvirtualIntMethodV", [&mem](Dynarmic::A32::Jit* cpu) {
            uint32_t methodID = cpu->Regs()[3];
            std::string key = MethodKey(methodID);

            if (key == "write([BII)I") {
                // AudioTrack.write(byte[] audioData, int offsetInBytes, int sizeInBytes)
                uint32_t va = mem.Read32(cpu->Regs()[13]);
                uint32_t byteArray = mem.Read32(va +  0);
                int32_t  offset    = static_cast<int32_t>(mem.Read32(va +  4));
                int32_t  length    = static_cast<int32_t>(mem.Read32(va +  8));

                if (byteArray && length > 0) {
                    const uint8_t* src = reinterpret_cast<const uint8_t*>(mem.GetHostPointer(byteArray)) + offset;
                    HLE::Audio::Write(src, length);
                }
                cpu->Regs()[0] = static_cast<uint32_t>(length); // bytes written
                return;
            }
            cpu->Regs()[0] = 0;
        });

        return env_ptr;
    }
};

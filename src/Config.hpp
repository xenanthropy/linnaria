#pragma once

#include <string_view>

// Compile-time configuration toggles. Defaults match "production" mode:
// fastmem on, most diagnostic prints off. Edit the values below and
// rebuild to change them. Because these are constexpr the compiler
// fully eliminates disabled print sites -- zero runtime cost when off.
//
// Always-on regardless of these flags:
//   - Error-class diagnostics ([ZLIB] ERROR, [Watchpoint] CROSS-THREAD,
//     OOB traps, [UNIMPLEMENTED] syscall calls).

namespace Config {

    namespace Performance {
        // Dynarmic's fastmem pointer path. With this on, memory accesses
        // go through emitted page-table arithmetic without a callback.
        // Massive perf win, but bypasses the OOB trap in EmuCallbacks and
        // the cross-thread Watchpoint check -- turn off when chasing
        // memory bugs.
        inline constexpr bool fastmem = true;

        // Dynarmic's very_verbose_debugging_output: extremely chatty
        // JIT trace, primarily useful for diagnosing mis-compiles.
        inline constexpr bool verboseDynarmic = false;
    }

    namespace Prints {
        // SyscallRouter "[Thread N] Executing: <symbol>" per-call trace.
        // Spammy enough to noticeably slow the terminal at 60 Hz.
        inline constexpr bool functionCalls = false;

        // Substring matched against the syscall name when functionCalls is
        // on. Names containing any of these are NOT printed -- useful for
        // muting high-frequency calls while tracing a specific behavior.
        // Add or remove freely.
        inline constexpr std::string_view functionCallMutes[] = {
            "pthread_mutex_lock",
            "pthread_mutex_unlock",
            "pthread_self",
            "JNI_PushLocalFrame",
            "JNI_PopLocalFrame",
            "JNI_GetMethodID",
            "JNI_FindClass",
        };

        // HLE_Zlib "[zlib] inflate(flush=...)" per-decompress trace.
        // [ZLIB] ERROR lines remain on regardless.
        inline constexpr bool zlib = false;

        // HLE_VFS "[VFS] fopen / fread / fwrite / lseek / open / write"
        // chatter for every file operation.
        inline constexpr bool vfs = false;

        // HLE_Android "[AssetManager] Opening: / FAILED / Allocated /
        // Magic Bytes" lines for each asset access.
        inline constexpr bool assetManager = false;

        // HLE_Android "[Android Log] <tag>: <body>" stream from the game's
        // __android_log_print calls. Default on -- this is where Octarine's
        // own diagnostics surface.
        inline constexpr bool androidLog = true;

        // Substring matched against the formatted Android log body when
        // androidLog is on. Lines containing any of these are NOT printed.
        // Defaults mute the per-touch chatter that floods during dragging.
        inline constexpr std::string_view androidLogMutes[] = {
            "RespondToTouchTrack",
            "TapCount:",
        };

        // Verbose mutex / CV trace, ungated by the functionCalls toggle.
        // When on, every pthread_mutex_{lock,trylock,unlock} and
        // pthread_cond_{wait,timedwait,signal,broadcast,destroy} prints a
        // line including the guest pointer and (for lock/unlock) the
        // calling thread id. Use this to diagnose deadlocks: grep by
        // pointer to see the full lifecycle of a single mutex.
        inline constexpr bool mutexTrace = false;

        // Watchdog heartbeat for the main thread. When on, a side host
        // thread wakes every ~1s and prints the main guest CPU's PC plus
        // a "stuck" counter (how many consecutive samples saw the same
        // PC). If the counter climbs past a few, main is in a guest-side
        // busy loop -- look up that PC in IDA to find which function.
        // The main loop also prints a nativeOnUpdate completion counter,
        // so if UserDefined1 stops firing entirely you know cpu.Run is
        // not returning at all.
        inline constexpr bool heartbeat = false;

        // Stack-corruption trap (requires fastmem OFF so writes route
        // through EmuCallbacks). Logs every guest write that stores a
        // page-aligned, code-pointer-shaped value (0x43000000-0x48000000)
        // into the top page of the main stack (0x7FEFF000-0x7FF00000) --
        // i.e. someone smashing a saved return address. Prints the writing
        // thread, the writer's PC, the target address, and the value.
        inline constexpr bool stackWriteTrap = false;
    }

}

#pragma once

// Compile-time configuration toggles. Defaults match "production" mode:
// fastmem on, most diagnostic prints off. Edit the values below and
// rebuild to change them. Because these are constexpr the compiler
// fully eliminates disabled print sites -- zero runtime cost when off.
//
// Always-on regardless of these flags:
//   - The Android log stream ([Android Log] ... lines).
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

        // HLE_Zlib "[zlib] inflate(flush=...)" per-decompress trace.
        // [ZLIB] ERROR lines remain on regardless.
        inline constexpr bool zlib = false;

        // HLE_VFS "[VFS] fopen / fread / fwrite / lseek / open / write"
        // chatter for every file operation.
        inline constexpr bool vfs = false;

        // HLE_Android "[AssetManager] Opening: / FAILED / Allocated /
        // Magic Bytes" lines for each asset access.
        inline constexpr bool assetManager = false;
    }

}

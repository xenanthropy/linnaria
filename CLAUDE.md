# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Linnaria is a Linux launcher/runtime for the Android APK build of Terraria 1.2.12785. It is **not** an Android emulator: it loads `libTerraria.so` (32-bit ARM), JITs the ARM code to x86_64 via **Dynarmic**, and exposes a minimal HLE layer for the Bionic/Android/JNI/OpenGL ES APIs the game touches. Host-only: Linux x86_64.

## Build / Run

```bash
git submodule update --init --recursive          # Dynarmic lives at third-party/dynarmic
cmake -B build -S .                              # add -DCMAKE_EXPORT_COMPILE_COMMANDS=YES for clangd
cmake --build build -j$(nproc)
build/linnaria lib/libTerraria.so                # user-supplied; APK assets must be extracted to ./assets, ./data, ./obb
```

Requires C++20 (Dynarmic), SDL2, zlib, and `glesv2` via pkg-config. `DYNARMIC_FRONTENDS` is pinned to `A32` in `CMakeLists.txt` — don't enable A64. There are no tests or linters wired up. If newer Xbyak is installed system-wide and Dynarmic miscompiles, apply `fix_xbyak.patch` (see BUILD.md).

A debug ThreadSanitizer build is plumbed but commented out in `CMakeLists.txt`. `config.very_verbose_debugging_output` and `config.fastmem_pointer` toggles in `src/main.cpp` are the usual knobs for diagnosing memory/JIT issues.

## Architecture

The pipeline is: `ElfLoader` maps the guest `.so` into `GuestMemory`, unresolved imports become SVC thunks, `EmuCallbacks` traps those SVCs and dispatches them via `SyscallRouter` to handlers registered by the `HLE::*` modules, and `main.cpp` drives the boot sequence + per-frame `nativeOnUpdate` loop.

**`GuestMemory`** (`src/GuestMemory.hpp`) reserves a 4 GiB virtual range with `mmap(PROT_NONE)` and commits:
- `CODE_BASE = 0x40000000` for 1 GiB of guest RAM (also the heap, bumped from `CODE_BASE + 0x10000000` via `AllocateHeap`, with a free-list for reuse).
- A kernel helper page at `0xFFFF0000` containing hand-assembled ARM machine code for `__kuser_cmpxchg` (0xFFFF0FC0) and `__kuser_memory_barrier` (0xFFFF0FA0).
- A ctype hack page at `0xEF000000` plus `AllocateCtypeArray()` which builds Bionic's `_ctype_`/`tolower`/`toupper` tables on demand.

The `page_table` array is handed to Dynarmic's fastmem path; `GetHostPointer` is the slow/safe fallback and returns a dummy cell + logs on OOB rather than crashing.

**`ElfLoader`** (`src/ElfLoader.{hpp,cpp}`) loads the ARM ELF, walks the dynamic section, and applies relocations. **Key trick:** every unresolved import is replaced by a small ARM "thunk" emitted in guest memory that executes `SVC #id`, where `id` is a per-symbol number stored in `id_to_name`. `EmuCallbacks::CallSVC` reverses the lookup and routes the call by string name. `GetThunk(name)` lets host code hand the guest a callable pointer for the same mechanism (this is how the JNI vtable and the magic `Emulator_Return_Trap` LR sentinel are built). `GetConstructors()` returns both `DT_INIT` and `DT_INIT_ARRAY` entries — main runs them all before invoking JNI.

**`EmuCallbacks`** (`src/EmuCallbacks.hpp`) implements `Dynarmic::A32::UserCallbacks`. SVC `0xFFFFFF` is reserved as the **pthread_once-done trap** and triggers `HaltReason::UserDefined3`; all other SVCs are symbol thunks. `PrintOOBMemoryRead` hard-exits on reads below `0x40000000` or at the `0xFFFFFFF4` Bionic stack-guard slot — these almost always indicate a missing HLE or a corrupted vtable.

**Halt-reason protocol** between guest code and `main.cpp`'s `ExecuteGameFunction` / main loop:
- `UserDefined1` — guest function returned (LR was set to the `Emulator_Return_Trap` thunk). Normal exit.
- `UserDefined2` — yield request from HLE (e.g. sleep/usleep). Main loop sleeps 1 ms and re-enters.
- `UserDefined3` — `pthread_once` finished its callback; restore `once_saved_state` (declared `thread_local` in `ThreadingHelpers.cpp`) so the original caller resumes.

**`SyscallRouter`** (`src/SyscallRouter.hpp`) maps symbol name → `std::function<void(Jit*)>`. Always register with the `ROUTE_REGISTER(router, "name", lambda)` macro — it captures `__FILE__`/`__LINE__` so `DumpSyscallMap("syscalls.txt")` (written on boot) doubles as an HLE coverage map. Unregistered calls log `[UNIMPLEMENTED]` and return 0 in R0; add the stub to the appropriate `HLE::*` module rather than `main.cpp`.

**HLE modules** (`src/HLE/HLE_*.hpp`) each expose `RegisterAll(router, memory, ...)` and group thunks by domain: `OS`, `Memory` (malloc/free over `GuestMemory::AllocateHeap`), `Strings`, `VFS` (routes through `HostAssetManager` for assets), `Math`, `Stdlib`, `Time`, `Threading`, `Network`, `Zlib`, `OpenGL` (forwards GLES2 calls to host via glad), `Android`. `AndroidEnvironment::RegisterAll` (`src/AndroidEnvironment.hpp`) is the single wire-up site.

**JNI** (`src/JNIEmulator.hpp`, `src/JNIFunctions.hpp`) constructs a fake `JNIEnv*` at `JNI_BASE = CODE_BASE + 0x8000000`: every JNI table slot is filled with a thunk that logs "unimplemented", then the slots the game actually uses are overwritten (`FindClass`=6, `GetMethodID`=33, `RegisterNatives`=215, `GetJavaVM`=219, etc.). A fake `JavaVM` lives 0x1000 bytes later with `AttachCurrentThread`/`GetEnv` wired up.

**Threading** (`src/HLE/HLE_Threading.hpp`) spawns a real host `std::thread` for each guest `pthread_create`, allocates a fresh guest stack + TLS page, builds its own `Dynarmic::A32::Jit` with a unique `processor_id` and an `AndroidCP15` configured for that TLS, and shares the `ExclusiveMonitor` allocated in `main.cpp` (256-slot capacity). `AndroidCP15` (`src/AndroidCP15.hpp`) only implements the read of `c13, c0, opc1=0, opc2=3` (TPIDRURO) — that's how Bionic finds the per-thread TLS pointer.

## Boot sequence in `main.cpp`

Knowing this order is essential when debugging crashes:
1. SDL2 + GLES2 context (1280×720), GLAD loader.
2. `GuestMemory`, `ElfLoader::Load`, `SyscallRouter`, `ExclusiveMonitor(256)`, `AndroidEnvironment::RegisterAll` (dumps `syscalls.txt`).
3. `EmuCallbacks` + Dynarmic `A32::Jit` for the main thread (`processor_id = 0`, ARMv7, fastmem disabled by default).
4. Run every `DT_INIT_ARRAY` constructor via `ExecuteGameFunction`.
5. Install JNI env (`JNIEmulator::Install`).
6. Call the Java→native bridge in order: `nativeOnCreateActivity` → `nativeOnSurfaceChanged` → `nativeOnResizeSurface` → (optional) `nativeOnExpansionFileExtracted` / `nativeUnlockGame` / `nativeOnResume`.
7. Main loop: poll SDL events, restore the saved `EmuThread` register file, `cpu.Run()`, save it back. On `UserDefined1` re-arm the registers to call `nativeOnUpdate(env, 0, 1, 1)` again, forcing CPSR Thumb bit because the return trap is ARM-mode.

## Conventions

- Guest pointers are `uint32_t`; host pointers come from `memory.GetHostPointer(vaddr)`. Don't pass host pointers into the guest.
- When adding an HLE call: pick or create the relevant `HLE::<Domain>` module, register with `ROUTE_REGISTER`, read args from `cpu->Regs()[0..3]` (stack for args 5+), write the return into `cpu->Regs()[0]`. Floats are bit-cast through `uint32_t` (see the `nativeOnResizeSurface` call site).
- The game is Thumb. When invoking guest code by address, preserve the low bit to set the Thumb flag in CPSR (the helpers in `main.cpp` do this).
- Don't commit anything under `lib/`, `assets/`, `data/`, `obb/`, `terraria_files/` — these are user-supplied and `.gitignore`d.

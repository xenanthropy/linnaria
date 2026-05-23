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

A debug ThreadSanitizer build is plumbed but commented out in `CMakeLists.txt`. All runtime knobs (fastmem, the Dynarmic verbose trace, every diagnostic print, mute lists, watchdog, stack-write trap, UAF probe, etc.) live in **`src/Config.hpp`** as `inline constexpr` toggles — edit the file, rebuild, that's it. The values are constexpr so disabled paths fully DCE away. Defaults are "production" (fastmem on, prints off, Android log on with touch chatter muted).

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
- `UserDefined1` — guest function returned (LR was set to the `Emulator_Return_Trap` thunk). Frame complete; in the main loop this is the only halt that sets `main_thread_clean = true` and breaks out of the inner `cpu.Run()` loop.
- `UserDefined2` — yield request from HLE (e.g. `nanosleep` / `usleep`). Main loop saves the guest's exact register state, breaks the inner loop, and lets SDL/idle yield for ~500 µs before next outer iteration restores and re-enters. Critically, `main_thread_clean` stays **false** so input dispatch waits.
- `UserDefined3` — `pthread_once` finished its callback; restore `once_saved_state` (declared `thread_local` in `ThreadingHelpers.cpp`) and **continue the inner loop** without breaking — the original caller resumes inline.
- Anything else (most commonly `halt == 0` = Dynarmic tick budget exhausted via `GetTicksRemaining`) also stays in the inner loop. The driver MUST treat budget exhaustion as "keep running" or a long guest function becomes hundreds of save/restore round-trips and runs ~300× slow.

**`SyscallRouter`** (`src/SyscallRouter.hpp`) maps symbol name → `std::function<void(Jit*)>`. Always register with the `ROUTE_REGISTER(router, "name", lambda)` macro — it captures `__FILE__`/`__LINE__` so `DumpSyscallMap("syscalls.txt")` (written on boot) doubles as an HLE coverage map. Unregistered calls log `[UNIMPLEMENTED]` and return 0 in R0; add the stub to the appropriate `HLE::*` module rather than `main.cpp`.

**HLE modules** (`src/HLE/HLE_*.hpp`) each expose `RegisterAll(router, memory, ...)` and group thunks by domain: `OS`, `Memory` (malloc/free over `GuestMemory::AllocateHeap`), `Strings`, `VFS` (routes through `HostAssetManager` for assets), `Math`, `Stdlib`, `Time`, `Threading`, `Network`, `Zlib`, `OpenGL` (forwards GLES2 calls to host via glad), `Android`. `AndroidEnvironment::RegisterAll` (`src/AndroidEnvironment.hpp`) is the single wire-up site.

**JNI** (`src/JNIEmulator.hpp`, `src/JNIFunctions.hpp`) constructs a fake `JNIEnv*` at `JNI_BASE = CODE_BASE + 0x8000000`: every JNI table slot is filled with a thunk that logs "unimplemented", then the slots the game actually uses are overwritten (`FindClass`=6, `GetMethodID`=33, `RegisterNatives`=215, `GetJavaVM`=219, etc.). A fake `JavaVM` lives 0x1000 bytes later with `AttachCurrentThread`/`GetEnv` wired up.

**Threading** (`src/HLE/HLE_Threading.hpp`) spawns a real host `std::thread` for each guest `pthread_create`, allocates a fresh guest stack + TLS page, builds its own `Dynarmic::A32::Jit` with a unique `processor_id` and an `AndroidCP15` configured for that TLS, and shares the `ExclusiveMonitor` allocated in `main.cpp` (256-slot capacity). `AndroidCP15` (`src/AndroidCP15.hpp`) only implements the read of `c13, c0, opc1=0, opc2=3` (TPIDRURO) — that's how Bionic finds the per-thread TLS pointer.

**`Watchpoint`** (`src/Watchpoint.hpp`) is a range-based diagnostic registry, not a feature: `HLE_Threading` adds/removes each guest pthread's stack range, `EmuCallbacks` calls `Find()` on every guest write to catch cross-thread writes into another thread's live stack, and `GuestMemory::AllocateHeap` checks `Find()` to detect heap blocks overlapping a still-live thread stack. Heap-vs-heap overlap is checked separately (always-on, O(log n) neighbor scan in `AllocateHeap`). Use these when chasing memory corruption that looks like a use-after-free or stack/heap collision.

**`Pacing`** (`src/Pacing.hpp`) — global `std::atomic`s read by the main loop and written by HLE modules on guest-state triggers:
- `game_tick_hz` (default 60) — how often the main loop invokes a guest tick. 0 = uncapped.
- `display_hz` (default 60) — how often the main loop calls `SDL_GL_SwapWindow`.
- `frame_dirty` — set by `glClear` / `glDrawElements` in `HLE_OpenGL`; cleared by the main loop on swap. Swap is gated on this so logic-only ticks don't ping-pong a stale back buffer.
- `input_enabled` — flipped true by `HLE_Android`'s `__android_log_print` handler when it sees the Octarine log line `"TerrariaInitializer::Run() DONE"` (= main menu fully constructed). Until then, `Input::HandleSDLEvent` drops SDL events at the gate so we don't dispatch into half-built game state. A second one-shot trigger on `"Initialized achievement system"` bumps `game_tick_hz` to 60 once boot is past asset extraction.

**`Input`** (`src/Input.hpp`) — translates SDL events to Octarine's JNI input. Single-finger touch (left mouse → `nativeTouchEvent` with action 0/1/2 = DOWN/UP/MOVE); keyboard via SDL keysym → AOSP `KeyEvent.KEYCODE_*` table → `nativeKeyEvent`. Special case: `keyCode == 66` (Enter) is rewritten to `(action=0, unicode='\n', keyCode=0)` because that's the magic 3-tuple Octarine's `onEditorAction` submits text with; see the in-game text-entry path. Events are queued in `std::deque`s; consecutive MOVEs coalesce to the latest position. `DrainPending` dispatches **at most one** touch + one key per main-loop iteration, gated on `main_thread_clean` — so `nativeOnUpdate` always drains the game's internal touch queue between additions. Dispatching a burst, or dispatching when `main_thread_clean` is false, corrupts the deque (see git log for the painful debugging session).

## Boot sequence in `main.cpp`

Knowing this order is essential when debugging crashes:
1. SDL2 + GLES2 context (1280×720), GLAD loader, `SDL_GL_SetSwapInterval(0)` (we own pacing in software).
2. `GuestMemory`, `ElfLoader::Load`, `SyscallRouter`, `ExclusiveMonitor(256)`, `AndroidEnvironment::RegisterAll` (dumps `syscalls.txt`).
3. `EmuCallbacks` + Dynarmic `A32::Jit` for the main thread (`processor_id = 0`, ARMv7). `config.fastmem_pointer` and `config.very_verbose_debugging_output` come from `Config::Performance::{fastmem, verboseDynarmic}`.
4. Run every `DT_INIT_ARRAY` constructor via `ExecuteGameFunction`.
5. Install JNI env (`JNIEmulator::Install`).
6. Call the Java→native bridge in order: `nativeOnCreateActivity` → `nativeOnSurfaceChanged` → `nativeOnResizeSurface` → (optional) `nativeOnExpansionFileExtracted` / `nativeUnlockGame` / `nativeOnResume`.
7. Main loop, per outer iteration:
   - Poll SDL events; F1–F4 toggle pacing locally, everything else goes to `Input::HandleSDLEvent` (drops if `Pacing::input_enabled == false`).
   - If `main_thread_clean`, call `Input::DrainPending` (dispatches at most one touch + one key via `ExecuteGameFunction`).
   - If `tick_due` and `main_thread.is_alive`: set `main_thread_clean = false`, restore the saved register file, then **inner `while (true)` loop**: call `cpu.Run()` and dispatch on halt — `UserDefined1` re-arms `nativeOnUpdate(env, 0, 1, 1)` (force Thumb CPSR=0x30 because the return trap is ARM), sets `main_thread_clean = true`, and `break`s; `UserDefined2` saves state and `break`s (clean stays false); `UserDefined3` restores `once_saved_state` and continues without breaking; **any other halt (including `halt == 0` for tick-budget exhaustion) clears and continues without breaking**. Skipping that "keep running" rule on tick-exhaustion is the trap that previously caused 5-second loads and mid-frame input dispatch corrupting the touch deque.
   - Swap only if `swap_due && Pacing::frame_dirty`. Idle-yield 500 µs if neither tick nor swap fired.

## Conventions

- Guest pointers are `uint32_t`; host pointers come from `memory.GetHostPointer(vaddr)`. Don't pass host pointers into the guest.
- When adding an HLE call: pick or create the relevant `HLE::<Domain>` module, register with `ROUTE_REGISTER`, read args from `cpu->Regs()[0..3]` (stack for args 5+), write the return into `cpu->Regs()[0]`. Floats are bit-cast through `uint32_t` (see the `nativeOnResizeSurface` call site).
- The game is Thumb. When invoking guest code by address, preserve the low bit to set the Thumb flag in CPSR (the helpers in `main.cpp` do this).
- Don't commit anything under `lib/`, `assets/`, `data/`, `obb/`, `terraria_files/` — these are user-supplied and `.gitignore`d.

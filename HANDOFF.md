# Handoff notes

This is a memory-transfer doc from the previous instance. Read it before you start; it's the stuff that doesn't belong in `CLAUDE.md` but the user wants you to have. Delete or trim freely once it's stale.

## What's working right now

- **Boot → main menu → character select → world creation → world generation** all run end to end. The 505 splash plays, Octarine reaches `TerrariaInitializer::Run() DONE`, the menu accepts taps.
- **Input**: single-finger touch (left mouse → DOWN/MOVE/UP) and keyboard (alphanumerics, arrows, modifiers, space/enter/tab/esc/backspace, common punctuation). Held drag works (the virtual joypad).
- **Speed**: clicking "Play" reaches character select roughly in line with the phone now (was previously 5 s due to the bug below). `game_tick_hz = 60` is the new default (was 0/uncapped); 60 plays nicer with the inner-loop driver than uncapped did.
- **Stability**: double-click during loading, dragging the color list in character creation, and exiting the credits screen — all of these used to corrupt the touch deque and freeze on a wild PC. Resolved by the main-loop rewrite (see "The bug we just killed" below).
- **HLE additions** that landed in this session: `fabsf`, `pow`, `qsort` (uses a temp Dynarmic JIT for the comparator — non-reentrant `Run()` workaround), `sprintf`, `glScissor`, `JNI_CallStaticBooleanMethodV` (slot 118), proper `pthread_cond_{wait,timedwait,signal,broadcast,destroy}`, `pthread_setspecific`/`getspecific`/`key_delete`, `pthread_mutex_trylock`. The user also fixed `pthread_key_create` to actually write the key (was silently broken).

## What's actively broken

In rough priority order:

1. **World load crashes.** World *generation* works; loading into a generated world or the tutorial crashes. No traces yet — this is where the user wants to focus next or near-next.
2. **Text input has a backspace bug.** In the "name your character" text bar, you can type until the game's char limit. Once at the limit, *no further keys work* — including backspace. Either we're going over the limit (off-by-one in our dispatch), or the game counts backspace as a key that's also blocked when the buffer is full. The user added a hack that rewrites SDL's Enter (`keyCode==66`) to `(action=0, unicode='\n', keyCode=0)` in `Input.hpp` so the text bar submits — that part works. The backspace-at-limit behavior is separate and unsolved.
3. **Extended worlds need `setMemoryInformation`.** The game checks RAM via `AndroidInterface::CheckMemoryInfo` and refuses extended worlds on devices it considers low-mem. We need to feed it a number large enough to unlock that path. Look in OctarineBridge.java for `setMemoryInformation(double, double)`; it's called from Java with real device numbers. We should call the same native bridge at boot with fake "plenty of RAM" values.
4. **Audio is completely absent.** Thread 7's tight loop (`pthread_mutex_lock → JNI_ReleasePrimitiveArrayCritical → JNI_CallNonvirtualIntMethodV → JNI_GetPrimitiveArrayCritical → pthread_mutex_unlock`) is the audio mixer poking `AudioTrack.write()` and getting 0 back. No backend wired. SDL_audio or PulseAudio would both work; SDL_audio is the obvious match since we already link SDL.
5. **AssetManager slow-path** — every miss in `./assets/` does a real `fopen` (and most asset names live in `./data/` or `./obb/`, so almost every lookup misses `./assets` first). On the phone the AssetManager is an in-memory hashtable lookup. Easy win: scan `./assets/` once at startup into an `unordered_set<string>`, short-circuit misses in `HostAssetManager::Open`. The user has been waiting on this; it'll noticeably tighten the splash sequence.

## Lower-priority observations the user has flagged

- The loading icon (`loading_icon.png`) never spins. Almost certainly because that's a Java `ImageView` overlay on top of the GLSurfaceView on Android, not anything the native code draws. We'd have to load the PNG ourselves and render it as a host-side quad during the early blackscreen.
- The credits screen has minor visual glitches (font characters missing per the Android log on the phone — game bug, not ours) and the back button vanishes when `game_tick_hz = 0` (uncapped) — animation-time related; ignore.
- Octarine logs `Connected to the internet!` *before* it tries to connect (and then fails). Cosmetic only; the failed sends are from RakNet trying to bring up multiplayer ports.
- `getFilesAtPath` returns 0 in `./data/` while the sibling `GetSDFilesAtPath` returns 7545 — there's likely a VFS dir-listing handler we got wrong. Doesn't crash anything currently but it's strange.
- We don't write `CONFIG.DAT` / `ACHIEVEMENT.DAT`. User suspects these are opened/closed just to test existence, and only get written on first achievement / config change. Won't know until we actually earn an achievement.

## The bug we just killed (read this before touching the main loop)

The double-click / drag freeze and the 300×-slow "Creating/Initializing player" were the **same bug**. My (the previous instance's) main loop called `cpu.Run()` **once per outer iteration**, then exited the tick block. But `cpu.Run()` returns whenever Dynarmic's per-call tick budget (`GetTicksRemaining() = 100000`) expires — that's a `halt_reason == 0` return, **not** a halt reason. For a long guest function (recipe init, asset load), this fired hundreds of times mid-execution, with full state save/restore and SDL polling and idle-yield between each. Hence 5 seconds for what's 16 ms on the phone.

Worse, after a budget-exhaustion return, `main_thread_clean` was still `true` from the previous *real* `UserDefined1`. So `DrainPending` would happily call `nativeTouchEvent` while the guest was mid-frame, smashing some random struct on the stack and turning the touch deque into garbage. We chased that as a heap UAF, then heap overlap, then JIT corruption — none of those — it was a state-machine bug in the driver. The user found the fix.

The current driver (in `main.cpp` around the `tick_due` block) has an **inner `while (true)` loop** that only `break`s on `UserDefined1` or `UserDefined2`. `UserDefined3` and any other halt (including budget exhaustion) clear and continue without breaking. `main_thread_clean` is set `false` on entry and only `true` on `UserDefined1`. **Do not regress this.** Anyone "simplifying" the loop back to a single `cpu.Run()` per iteration will resurrect both the perf bug and the deque corruption.

## Diagnostic infrastructure that's still around

All in `Config.hpp`, all default off. Worth knowing what's available before you spend an hour building something that already exists:

- `Performance::fastmem` — Dynarmic's fastmem pointer path. Off makes the OOB trap + cross-thread Watchpoint check live, ~5–10× slower.
- `Performance::verboseDynarmic` — JIT trace, deafening, only useful for diagnosing mis-compiles.
- `Performance::disableHeapReuse` — makes `FreeHeap` a no-op (pure bump allocator). Set this if you suspect a UAF: if a crash disappears, you've confirmed one.
- `Prints::functionCalls` + `functionCallMutes[]` — the `[Thread N] Executing: foo` syscall trace, with a substring mute list so the noisy pthread/JNI calls don't drown the signal. Edit the array, rebuild, no need to chase down each callsite.
- `Prints::androidLog` + `androidLogMutes[]` — same idea for the `[Android Log]` stream. Touch chatter is muted by default.
- `Prints::zlib` / `vfs` / `assetManager` / `miscPrints` — per-domain prints.
- `Prints::mutexTrace` — per-call lock/unlock/wait/signal log with guest pointers. Grep by pointer to follow one mutex's lifecycle across threads. Used heavily during the freeze debugging.
- `Prints::heartbeat` — main-loop watchdog. A background thread samples the main guest CPU's PC every second and prints it plus a "stuck for N samples" counter. Critical for diagnosing wild jumps — when PC is unchanged for 3+ samples, it dumps all 16 registers + 24 stack words. Reading LR (R14) and the stacked return addresses against IDA is how we found the touch path was the crash site.
- `Prints::stackWriteTrap` — Catches writes of float-coordinate-shaped values into the top of the main stack. Useful but unreliable: Dynarmic services most writes through the page table, bypassing our `MemoryWrite32` callback. Treat hits as one signal among many.
- The `AllocateHeap` overlap self-check is **always on** (cheap O(log n) neighbor scan). If it ever fires it prints both caller PCs and exits — that's a confirmed allocator bug.

## Coding conventions specific to this codebase (worth absorbing)

- Float args are bit-cast through `uint32_t` for the register file. Doubles use a register pair, low half in the lower-numbered register (soft-float AAPCS). See `pow` in `HLE_Math.hpp` for the pattern, or `sqrt` for the same shape on a unary double.
- Adding an HLE: pick the relevant `HLE/HLE_<Domain>.hpp`, register with the `ROUTE_REGISTER` macro (it captures `__FILE__`/`__LINE__` so the `syscalls.txt` dump doubles as coverage).
- Diagnostic prints belong behind a `Config::Prints::*` constexpr toggle. They DCE away when off, so there's no cost to leaving them in.
- Don't lock `console_mutex` inside `allocator_mutex` (or vice versa) — the existing pattern is "capture data under the lock, log + dump outside". `DumpAllocationsNear` re-locks `allocator_mutex`, so calling it from inside a holding context deadlocks.
- The qsort pattern is worth remembering: if you need to call guest code from inside an HLE handler (which is itself inside `cpu.Run()`), Dynarmic's `Run()` is non-reentrant. The fix is to spin up a temporary `Dynarmic::A32::Jit` with a unique `processor_id` (we use 999) and a scratch stack via `AllocateHeap`. Don't try to re-enter the calling cpu's `Run()`.
- When you reset the main thread's register file for a fresh `nativeOnUpdate`, force CPSR Thumb bit (`main_thread.cpsr = 0x30`). The return trap is ARM-mode so the saved CPSR will be ARM when you come back, but nativeOnUpdate is Thumb.

## A few traps with input dispatch specifically

These caused us pain; reading them once will save you the round-trip:

- `DrainPending` **must** be gated on `main_thread_clean`. If you ever find yourself wanting to remove that gate, don't — the game's `fjAddTouchEvent` runs on the main stack, and the previous frame's state has to be fully unwound first.
- Touch dispatch is one event per outer iteration; coalesce consecutive MOVEs in `HandleSDLEvent` for the same pointer ID. If you batch-dispatch many events in one drain, the game's internal touch queue can fill before `nativeOnUpdate` drains it.
- `ExecuteGameFunction` reuses the main thread's stack at `CODE_BASE + MEMORY_SIZE - 0x100000`. This is fine *only* between `nativeOnUpdate` calls — i.e. exactly when `main_thread_clean == true`. Don't call `ExecuteGameFunction` from anywhere else on the main thread.

## Coordinates / data values worth recognizing

Float screen coordinates land in the `0x42xxxxxx`–`0x44xxxxxx` range as raw bits, and many are page-aligned (e.g. `0x43860000 = 268.0f`, `0x44200000 = 640.0f`). If you see "page-aligned garbage" in a debug dump around those magnitudes, it might actually be a touch coordinate that's gotten somewhere it shouldn't.

## Communication style the user prefers

- Substantive but not bloated. They're technical and engaged — feed them the actual data, not vague gestures.
- They will paste IDA disassembly when asked. When you ask for it, name exactly what you want (function name + a few lines around the call site, not "the whole binary").
- They're fine with diagnostic-only commits ("add a trap to catch X") as long as you call out that it's diagnostic and what the next step is.
- When a fix lands, they appreciate a clear "what changed, why, what to test, what to watch out for" recap. Match that.

# Linnaria

Linnaria is a launcher/runtime that enables the Android APK version of Terraria (v1.2.12785) to run on Linux systems.

The goal of the project is to make the last mobile release containing old-console exclusive content playable on PC in a mostly native-like way, using dynamic recompilation via Dynarmic.

> Note: This is not a traditional emulator, but also not truly native — it translates ARM instructions to the host architecture at runtime.

---

## Features

- Runs Terraria Mobile 1.2.12785 on Linux
- Preserves old-console exclusive content
- Uses Dynarmic for ARM → x64 translation
- Focused on performance and simplicity over full Android emulation

---

## Status

This project is currently a massive work in progress and is **not** in a playable state.

Expect bugs, missing features, and rough edges.

---

## How It Works

Linnaria loads the original Terraria Android library and executes it using a custom runtime layer.

Instead of emulating a full Android environment, it:
- Translates ARM instructions using Dynarmic
- Provides minimal implementations ("thunks") for required Android/Bionic APIs
- Bridges system calls to native Linux equivalents where possible

---

## Requirements

- Linux (x86_64 only, sorry)
- Terraria APK (version 1.2.12785)
- Dependencies (Zlib, SDL2, more TBD)

---

## Usage

> ⚠️ This section is incomplete and will change.

First, extract the assets folder from the APK and place in the root directory. Then, extract the libTerraria.so library from the APK and place it in the lib/ folder in the root directory.
Then:
```bash
# Example (from root dir)
build/Linnaria lib/libTerraria.so
```
---

## Legal Notice

Linnaria does not provide Terraria or any game assets.

You must supply your own legally obtained copy of the Terraria APK. (Hopefully it's **archive**d somewhere, perhaps by an **org** or something?)

---

## Contributing

Contributions are welcome!

If you're interested in helping:

1. Fork the repository
2. Create a new branch (git checkout -b feature/your-feature)
3. Make your changes
4. Commit (git commit -m "Add some feature")
5. Push (git push origin feature/your-feature)
6. Open a Pull Request

## Guidelines
Keep code readable and consistent (even if my own isn't)
Prefer small, focused commits
Document non-obvious behavior
Be mindful of platform-specific behavior (ARM vs x86)

---

### Goals
 - [ ] Boot the game reliably
 - [ ] Implement required Bionic/libc functions
 - [ ] Basic rendering/input support
 - [ ] Playable gameplay
 - [ ] Performance improvements

### Non-Goals
❌ Full Android emulation
❌ Supporting modern Terraria versions (defeats the purpose of the project)
❌ Perfect compatibility with all Android APIs

### Acknowledgements
Dynarmic (dynamic recompilation engine)
Terraria (Re-Logic/codeglue)

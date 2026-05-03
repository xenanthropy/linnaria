# Building Linnaria

## Requirements

* Linux (x86_64 only)
* CMake (3.16+ recommended)
* A C++ compiler with C++20 support (minimum required for dynarmic)
* Git

### Dependencies

* SDL2
* zlib

> Note: `libpng` is typically available on most systems and may be required indirectly.

---

## Cloning the Repository

Linnaria uses submodules (e.g. Dynarmic), so make sure to clone recursively:

```bash
git clone --recursive https://github.com/xenanthropy/linnaria.git
cd linnaria
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

---

## Building

### Configure

```bash
cmake -B build -S .
```

If you're developing and want `compile_commands.json`:

```bash
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=YES -B build -S .
```

### Build

```bash
cmake --build build -j$(nproc)
```

---

## Notes on Dynarmic

Linnaria includes Dynarmic as a subproject.

A patch is provided in the root dir (`fix_xbyak.patch`).

This patch updates Dynarmic’s CMake configuration to fix an issue with newer versions of Xbyak.

In most cases, you won't need this unless you are modifying Dynarmic or updating its dependencies. (Dynarmic's included Xbyak is fine, if you have a newer version of Xbyak installed externally you **will** need this patch or else Dynarmic won't work properly).

To patch (run in root dir):
```bash
patch -p1 < fix_zbyak.patch
```

---

## Troubleshooting

### Missing SDL2 or zlib

Install via your package manager:

**Arch Linux:**

```bash
sudo pacman -S sdl2 zlib
```

**Debian/Ubuntu:**

```bash
sudo apt install libsdl2-dev zlib1g-dev
```

---

## Development Notes

* `compile_commands.json` is optional and only needed for tooling (clangd, etc.)
* The project is under active development; build instructions may change


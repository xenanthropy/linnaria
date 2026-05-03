#include "ElfLoader.hpp"
#include <iostream>
#include <fstream>
#include <cstring>

ElfLoader::ElfLoader(GuestMemory& memory) : mem(memory) {}

uint32_t ElfLoader::ctype_array_ptr = 0;

bool ElfLoader::Load(const std::string& filepath) {
    std::cout << "Loading ELF: " << filepath << std::endl;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open " << filepath << std::endl;
        return false;
    }

    Elf32_Ehdr ehdr;
    file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));

    if (memcmp(ehdr.e_ident, ELFMAG, 4) != 0 || ehdr.e_machine != EM_ARM) {
        std::cerr << "Invalid ELF or not ARM architecture!" << std::endl;
        return false;
    }

    std::vector<Elf32_Phdr> phdrs(ehdr.e_phnum);
    file.seekg(ehdr.e_phoff);
    file.read(reinterpret_cast<char*>(phdrs.data()), sizeof(Elf32_Phdr) * ehdr.e_phnum);

    uint32_t min_addr = 0xFFFFFFFF;
    uint32_t max_addr = 0;

    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_LOAD) {
            if (phdr.p_vaddr < min_addr) min_addr = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > max_addr) max_addr = phdr.p_vaddr + phdr.p_memsz;
        }
    }

    min_addr &= ~0xFFF;
    max_addr = (max_addr + 0xFFF) & ~0xFFF;
    load_bias = GuestMemory::CODE_BASE - min_addr;
    image_end = GuestMemory::CODE_BASE + (max_addr - min_addr);

    // Load segments into memory
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_LOAD) {
            uint32_t seg_start = phdr.p_vaddr + load_bias;
            
            // Read file data
            std::vector<uint8_t> segment_data(phdr.p_filesz);
            file.seekg(phdr.p_offset);
            file.read(reinterpret_cast<char*>(segment_data.data()), phdr.p_filesz);
            
            // Write to guest memory
            for (size_t i = 0; i < phdr.p_filesz; i++) {
                mem.Write8(seg_start + i, segment_data[i]);
            }
        }
    }

    // Process Dynamic tags (PLT, GOT, etc.)
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == PT_DYNAMIC) {
            ProcessDynamicSection(phdr.p_vaddr + load_bias);
            ApplyRelocations();
            break;
        }
    }

    entry_point = ehdr.e_entry + load_bias;
    std::cout << "ELF loaded successfully. Entry point: 0x" << std::hex << entry_point << std::dec << std::endl;
    
    return true;
}

void ElfLoader::ProcessDynamicSection(uint32_t dyn_addr) {
    while (true) {
        uint32_t tag = mem.Read32(dyn_addr);
        uint32_t val = mem.Read32(dyn_addr + 4);
        if (tag == DT_NULL) break;

        switch (tag) {
            case DT_STRTAB: dyn.strtab = val + load_bias; break;
            case DT_SYMTAB: dyn.symtab = val + load_bias; break;
            case DT_HASH:   dyn.hash   = val + load_bias; break;
            case DT_REL:    dyn.rel    = val + load_bias; break;
            case DT_RELSZ:  dyn.relsz  = val; break;
            case DT_JMPREL: dyn.jmprel = val + load_bias; break;
            case DT_PLTRELSZ: dyn.pltrelsz = val; break;
        }
        dyn_addr += 8;
    }
}

void ElfLoader::InitTrampolinePage() {
    if (tramp_next != 0) return;
    // Align image end to next page boundary and carve out space for trampolines
    tramp_next = (image_end + 0xFFF) & ~0xFFF;
    image_end = tramp_next + 0x10000; // Give it 64KB of trampoline space
}

uint32_t ElfLoader::EmitSVCThunk(uint32_t id) {
    InitTrampolinePage();
    uint32_t addr = tramp_next;
    
    uint32_t svc = 0xEF000000 | (id & 0x00FFFFFF); // SVC #id
    uint32_t bxlr= 0xE12FFF1E;                     // BX LR (Return)
    
    mem.Write32(addr + 0, svc);
    mem.Write32(addr + 4, bxlr);
    
    tramp_next += 8;
    return addr;
}

uint32_t ElfLoader::ResolveLocal(uint32_t sym_index) {
    if (!dyn.symtab) return 0;
    auto* syms = reinterpret_cast<const Elf32_Sym*>(mem.GetHostPointer(dyn.symtab));
    if (syms[sym_index].st_value) {
        return load_bias + syms[sym_index].st_value;
    }
    return 0;
}

std::string ElfLoader::GetDynSymName(uint32_t sym_index) {
    if (!dyn.symtab || !dyn.strtab) return "";
    auto* syms = reinterpret_cast<const Elf32_Sym*>(mem.GetHostPointer(dyn.symtab));
    const char* str = reinterpret_cast<const char*>(mem.GetHostPointer(dyn.strtab + syms[sym_index].st_name));
    return str ? std::string(str) : "";
}

uint32_t ElfLoader::ResolveOrThunk(const std::string& name) {
    // Special case: ctype arrays
    if (name == "_ctype_" || name == "__ctype_" || name == "__ctype_ptr__") {
        if (ctype_array_ptr == 0) {
            ctype_array_ptr = mem.AllocateCtypeArray();
        }
        return ctype_array_ptr;
    }

    if (name.empty()) return 0;
    auto it = name_to_thunk.find(name);
    if (it != name_to_thunk.end()) return it->second;
    
    uint32_t id = next_svc_id++;
    uint32_t thunk = EmitSVCThunk(id);
    name_to_thunk[name] = thunk;
    id_to_name[id] = name;
    
    return thunk;
}

void ElfLoader::ApplyRel(const Elf32_Rel* rel, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t type = ELF32_R_TYPE(rel[i].r_info);
        uint32_t sym  = ELF32_R_SYM(rel[i].r_info);
        uint32_t where= rel[i].r_offset + load_bias;
        uint32_t addend = mem.Read32(where);

        switch (type) {
            case R_ARM_RELATIVE:
                mem.Write32(where, load_bias + addend);
                break;
            case R_ARM_ABS32: {
                uint32_t s = sym ? ResolveLocal(sym) : 0;
                if (sym && !s) s = ResolveOrThunk(GetDynSymName(sym));
                mem.Write32(where, s + addend);
                break;
            }
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                std::string name = GetDynSymName(sym);
                uint32_t tgt = ResolveLocal(sym);
                if (!tgt) tgt = ResolveOrThunk(name);
                mem.Write32(where, tgt);
                break;
            }
        }
    }
}

void ElfLoader::ApplyRelocations() {
    if (dyn.rel && dyn.relsz) {
        auto* r = reinterpret_cast<const Elf32_Rel*>(mem.GetHostPointer(dyn.rel));
        ApplyRel(r, dyn.relsz / sizeof(Elf32_Rel));
    }
    if (dyn.jmprel && dyn.pltrelsz) {
        auto* r = reinterpret_cast<const Elf32_Rel*>(mem.GetHostPointer(dyn.jmprel));
        ApplyRel(r, dyn.pltrelsz / sizeof(Elf32_Rel));
    }
    std::cout << "[Relocations] Bound PLT/GOT tables to trampolines." << std::endl;
}

uint32_t ElfLoader::GetExport(const std::string& name) {
    if (!dyn.symtab || !dyn.strtab) return 0;
    
    // Use the DT_HASH table to figure out how many symbols exist
    uint32_t num_symbols = 1 << 16; // Generous fallback
    if (dyn.hash) {
        const uint32_t* hash_table = reinterpret_cast<const uint32_t*>(mem.GetHostPointer(dyn.hash));
        num_symbols = hash_table[1]; 
    }

    auto* syms = reinterpret_cast<const Elf32_Sym*>(mem.GetHostPointer(dyn.symtab));
    for (uint32_t i = 0; i < num_symbols; i++) {
        if (syms[i].st_name == 0) continue;
        
        const char* sym_name = reinterpret_cast<const char*>(mem.GetHostPointer(dyn.strtab + syms[i].st_name));
        
        // If the name matches and it's an actual function
        if (name == sym_name && syms[i].st_value != 0) {
            return load_bias + syms[i].st_value;
        }
    }
    return 0;
}

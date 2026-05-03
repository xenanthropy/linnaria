#pragma once
#include "GuestMemory.hpp"
#include <string>
#include <elf.h>
#include <unordered_map>
#include <vector>

struct DynInfo {
    uint32_t strtab = 0;
    uint32_t symtab = 0;
    uint32_t rel = 0, relsz = 0;
    uint32_t jmprel = 0, pltrelsz = 0;
    uint32_t hash = 0;
};

class ElfLoader {
public:
    ElfLoader(GuestMemory& memory);
    bool Load(const std::string& filepath);
    
    uint32_t GetEntryPoint() const { return entry_point; }
    uint32_t GetLoadBias() const { return load_bias; }
    uint32_t GetImageEnd() const { return image_end; }
    uint32_t GetExport(const std::string& name);
    uint32_t GetThunk(const std::string& name) { return ResolveOrThunk(name); }

    // We need this so Dynarmic can translate an SVC ID back to a string name
    std::string GetSymbolName(uint32_t svc_id) {
        auto it = id_to_name.find(svc_id);
        return it != id_to_name.end() ? it->second : "";
    }

private:
    void ProcessDynamicSection(uint32_t dyn_addr);
    void ApplyRelocations();

    GuestMemory& mem;
    uint32_t entry_point = 0;
    uint32_t load_bias = 0;
    uint32_t image_end = 0;
    DynInfo dyn;

    uint32_t tramp_next = 0;
    uint32_t next_svc_id = 1;

    static uint32_t ctype_array_ptr;
    
    std::unordered_map<std::string, uint32_t> name_to_thunk;
    std::unordered_map<uint32_t, std::string> id_to_name;

    void InitTrampolinePage();
    uint32_t EmitSVCThunk(uint32_t id);
    uint32_t ResolveLocal(uint32_t sym_index);
    std::string GetDynSymName(uint32_t sym_index);
    uint32_t ResolveOrThunk(const std::string& name);
    void ApplyRel(const Elf32_Rel* rel, size_t count);
};

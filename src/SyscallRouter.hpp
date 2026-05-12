#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <dynarmic/interface/A32/a32.h>
#include <vector>

using SyscallHandler = std::function<void(Dynarmic::A32::Jit* cpu)>;

class SyscallRouter {
public:

    void Register(const std::string& name, SyscallHandler handler) {
        handlers[name] = handler;
    }

    void Invoke(const std::string& name, Dynarmic::A32::Jit* cpu) {
        auto it = handlers.find(name);
        if (it != handlers.end()) {
            std::cout << "[Router] Executing: " << name <<  std::endl;
            it->second(cpu);
        } else {
            // The "Catch-All" stub
            std::cout << "[UNIMPLEMENTED] Game called: " << name << std::endl;
            
            // Optionally force a crash or return 0
            cpu->Regs()[0] = 0; 
        }
    }

private:
    std::unordered_map<std::string, SyscallHandler> handlers;
};

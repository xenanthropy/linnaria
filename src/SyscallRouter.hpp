#pragma once
#include "GuestMemory.hpp"
#include "CPUHelper.hpp"
#include "Config.hpp"
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <dynarmic/interface/A32/a32.h>

using SyscallHandler = std::function<void(Dynarmic::A32::Jit* cpu)>;

struct SyscallInfo {
    SyscallHandler handler;
    std::string file_path;
    int line_number;
};

static std::mutex console_mutex;

class SyscallRouter {
public:
    void RegisterInternal(const std::string& name, const char* file, int line, SyscallHandler handler) {
        // Strip the long absolute paths (e.g., /home/user/workspace/src/...) to just the filename
        std::string filename = file;
        size_t slash_pos = filename.find_last_of("/\\");
        if (slash_pos != std::string::npos) {
            filename = filename.substr(slash_pos + 1);
        }
        handlers[name] = {handler, filename, line};
    }

    void Invoke(const std::string& name, Dynarmic::A32::Jit* cpu) {
        auto it = handlers.find(name);
        if (it != handlers.end()) {
            if constexpr (Config::Prints::functionCalls) {
                bool muted = false;
                for (auto m : Config::Prints::functionCallMutes) {
                    if (name.find(m) != std::string::npos) { muted = true; break; }
                }
                if (!muted) {
                    std::lock_guard<std::mutex> lock(console_mutex);
                    std::cout << "[Thread " << active_thread_id << "] Executing: " << name << "\n";
                }
            }
            it->second.handler(cpu);
        } else {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "[UNIMPLEMENTED] Game called: " << name << std::endl;
            cpu->Regs()[0] = 0;
        }
    }

    void DumpSyscallMap(const std::string& filepath) {
        std::ofstream out(filepath);
        if (!out) return;

        out << "========================================\n";
        out << "        LINNARIA SYSCALL MAP            \n";
        out << "========================================\n\n";
        out << std::left << std::setw(35) << "Function Name" 
            << std::setw(30) << "Source File" 
            << "Line\n";
        out << std::string(75, '-') << "\n";

        for (const auto& [name, info] : handlers) {
            out << std::left << std::setw(35) << name 
                << std::setw(30) << info.file_path 
                << info.line_number << "\n";
        }
        std::cout << "[Router] Saved syscall map to " << filepath << std::endl;
    }

    SyscallHandler GetHandler(const std::string& name) {
        auto it = handlers.find(name);
        return it != handlers.end() ? it->second.handler : nullptr;
    }

private:
    std::unordered_map<std::string, SyscallInfo> handlers;
};

// --- THE MAGIC MACRO ---
#define ROUTE_REGISTER(router_obj, name, ...) \
    router_obj.RegisterInternal(name, __FILE__, __LINE__, __VA_ARGS__)

#pragma once
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
    // Notice the added file and line arguments
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
            //std::cout << "[TID: " << std::this_thread::get_id() << "] "
            //          << "Executing: " << name << " (from " << it->second.file_path << ")" << std::endl;
            // Optional: You can print the file it came from here too!
            //std::cout << "[Router] Executing: " << name << " (from " << it->second.file_path << ")" << std::endl;
            // OPTIONAL: Uncomment to see successful calls 
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[TID: " << std::this_thread::get_id() << "] Executing: " << name << "\n";
            }
            it->second.handler(cpu);
        } else {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "[UNIMPLEMENTED] Game called: " << name << std::endl;
            cpu->Regs()[0] = 0;
        }
    }

    // Call this right after you finish registering everything in main.cpp!
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

private:
    std::unordered_map<std::string, SyscallInfo> handlers;
};

// --- THE MAGIC MACRO ---
// Use this instead of router.Register() so it automatically injects __FILE__ and __LINE__
#define ROUTE_REGISTER(router_obj, name, ...) \
    router_obj.RegisterInternal(name, __FILE__, __LINE__, __VA_ARGS__)

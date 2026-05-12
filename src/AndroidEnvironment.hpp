#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include <dynarmic/interface/exclusive_monitor.h>

#include "HLE/HLE_OS.hpp"
#include "HLE/HLE_Memory.hpp"
#include "HLE/HLE_Strings.hpp"
#include "HLE/HLE_VFS.hpp"
#include "HLE/HLE_Math.hpp"
#include "HLE/HLE_Stdlib.hpp"
#include "HLE/HLE_Time.hpp"
#include "HLE/HLE_Threading.hpp"
#include "HLE/HLE_Network.hpp"
#include "HLE/HLE_Zlib.hpp"
#include "HLE/HLE_OpenGL.hpp"
#include "HLE/HLE_Android.hpp"

struct EmuThread {
    std::array<uint32_t, 16> regs;
    uint32_t cpsr;
    bool is_alive = true;
};

class AndroidEnvironment {

public:
    static void RegisterAll(SyscallRouter& router, GuestMemory& memory, ElfLoader& loader, Dynarmic::ExclusiveMonitor& monitor) {
        
        HLE::OS::RegisterAll(router, memory);
        HLE::Memory::RegisterAll(router, memory);
        HLE::Strings::RegisterAll(router, memory);
        HLE::VFS::RegisterAll(router, memory);
        HLE::Math::RegisterAll(router, memory);
        HLE::Stdlib::RegisterAll(router, memory);
        HLE::Time::RegisterAll(router, memory);
        HLE::Threading::RegisterAll(router, memory, loader, monitor);
        HLE::Network::RegisterAll(router, memory);
        HLE::Zlib::RegisterAll(router, memory);
        HLE::OpenGL::RegisterAll(router, memory);
        HLE::Android::RegisterAll(router, memory);

    }
};

#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace HLE::Network {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        // TODO: listen, accept, connect, send, recv, recvfrom, shutdown, getsockopt, poll
        //       select, inet_ntoa, getaddrinfo, freeaddrinfo, gai_strerror, getnameinfo, if_nametoindex

        ROUTE_REGISTER(router, "socket", [](Dynarmic::A32::Jit* cpu) {
            int domain   = static_cast<int>(cpu->Regs()[0]);
            int type     = static_cast<int>(cpu->Regs()[1]);
            int protocol = static_cast<int>(cpu->Regs()[2]);
            int fd       = ::socket(domain, type, protocol);
            cpu->Regs()[0] = fd;
        });

        ROUTE_REGISTER(router, "bind", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd       = static_cast<int>(cpu->Regs()[0]);
            uint32_t addr_ptr = cpu->Regs()[1];
            uint32_t addrlen  = cpu->Regs()[2];
            if (!addr_ptr) { cpu->Regs()[0] = -1; return; }

            const void* addr = memory.GetHostPointer(addr_ptr);
            int ret = ::bind(sockfd, static_cast<const struct sockaddr*>(addr), addrlen);
            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "sendto", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd          = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf_ptr     = cpu->Regs()[1];
            uint32_t len         = cpu->Regs()[2];
            int flags            = static_cast<int>(cpu->Regs()[3]);
            uint32_t dest_ptr    = cpu->Regs()[4];
            uint32_t addrlen     = cpu->Regs()[5];
            if (!buf_ptr || !dest_ptr) { cpu->Regs()[0] = -1; return; }

            const void* buf  = memory.GetHostPointer(buf_ptr);
            const void* dest = memory.GetHostPointer(dest_ptr);

            ssize_t ret = ::sendto(sockfd, buf, len, flags,
                                   static_cast<const struct sockaddr*>(dest), addrlen);
            cpu->Regs()[0] = static_cast<uint32_t>(ret);
        });

        ROUTE_REGISTER(router, "getsockname", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd            = static_cast<int>(cpu->Regs()[0]);
            uint32_t addr_ptr      = cpu->Regs()[1];
            uint32_t addrlen_ptr   = cpu->Regs()[2];
            if (!addr_ptr || !addrlen_ptr) { cpu->Regs()[0] = -1; return; }

            void* addr = memory.GetHostPointer(addr_ptr);
            socklen_t len = memory.Read32(addrlen_ptr);

            int ret = ::getsockname(sockfd, static_cast<struct sockaddr*>(addr), &len);
            memory.Write32(addrlen_ptr, static_cast<uint32_t>(len));
            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "setsockopt", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd    = static_cast<int>(cpu->Regs()[0]);
            int level     = static_cast<int>(cpu->Regs()[1]);
            int optname   = static_cast<int>(cpu->Regs()[2]);
            uint32_t optval_ptr = cpu->Regs()[3];
            uint32_t optlen     = cpu->Regs()[4];
            if (!optval_ptr) { cpu->Regs()[0] = -1; return; }

            const void* optval = memory.GetHostPointer(optval_ptr);
            int ret = ::setsockopt(sockfd, level, optname, optval, optlen);
            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "inet_addr", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t cp_ptr = cpu->Regs()[0];
            if (!cp_ptr) { cpu->Regs()[0] = INADDR_NONE; return; }

            const char* cp = reinterpret_cast<const char*>(memory.GetHostPointer(cp_ptr));
            cpu->Regs()[0] = ::inet_addr(cp);
        });

        ROUTE_REGISTER(router, "gethostbyname", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t name_ptr = cpu->Regs()[0];
            if (!name_ptr) { cpu->Regs()[0] = 0; return; }

            const char* name = reinterpret_cast<const char*>(memory.GetHostPointer(name_ptr));
            struct hostent* he = ::gethostbyname(name);
            if (!he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0]) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Simplified guest copy: only h_name, h_addrtype, h_length, and one IPv4 in h_addr_list.
            uint32_t name_len = strlen(he->h_name) + 1;
            uint32_t total    = 20 + name_len + 8 + 4; // hostent + name + addrlist[2] + addr
            uint32_t base     = memory.AllocateHeap(total);

            uint32_t name_guest      = base + 20;
            uint32_t addr_list_guest = name_guest + name_len;
            uint32_t addr_guest      = addr_list_guest + 8;

            memcpy(memory.GetHostPointer(name_guest), he->h_name, name_len);
            memory.Write32(addr_list_guest + 0, addr_guest);
            memory.Write32(addr_list_guest + 4, 0);
            memcpy(memory.GetHostPointer(addr_guest), he->h_addr_list[0], 4);

            memory.Write32(base + 0,  name_guest);      // h_name
            memory.Write32(base + 4,  0);               // h_aliases = NULL
            memory.Write32(base + 8,  he->h_addrtype);  // h_addrtype
            memory.Write32(base + 12, he->h_length);    // h_length
            memory.Write32(base + 16, addr_list_guest); // h_addr_list

            cpu->Regs()[0] = base;
        });

        ROUTE_REGISTER(router, "gethostname", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t name_ptr = cpu->Regs()[0];
            uint32_t len      = cpu->Regs()[1];
            if (!name_ptr) { cpu->Regs()[0] = -1; return; }

            char* name = reinterpret_cast<char*>(memory.GetHostPointer(name_ptr));
            cpu->Regs()[0] = ::gethostname(name, len);
        });


    }
}


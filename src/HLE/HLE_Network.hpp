#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace HLE::Network {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

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
            int sockfd           = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf_ptr     = cpu->Regs()[1];
            uint32_t len         = cpu->Regs()[2];
            int flags            = static_cast<int>(cpu->Regs()[3]);
            
            // Arguments 5 and 6 are passed on the stack
            uint32_t sp          = cpu->Regs()[13];
            uint32_t dest_ptr    = memory.Read32(sp);
            uint32_t addrlen     = memory.Read32(sp + 4);

            if (!buf_ptr || !dest_ptr) { cpu->Regs()[0] = -1; return; }

            const void* buf  = memory.GetHostPointer(buf_ptr);
            const void* dest = memory.GetHostPointer(dest_ptr);

            ssize_t ret = ::sendto(sockfd, buf, len, flags,
                                   static_cast<const struct sockaddr*>(dest), addrlen);
            cpu->Regs()[0] = static_cast<uint32_t>(ret);
        });

        ROUTE_REGISTER(router, "recvfrom", [&memory](Dynarmic::A32::Jit* cpu) {
            int sockfd           = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf_ptr     = cpu->Regs()[1];
            uint32_t len         = cpu->Regs()[2];
            int flags            = static_cast<int>(cpu->Regs()[3]);

            // Arguments 5 and 6 are passed on the stack
            uint32_t sp          = cpu->Regs()[13];
            uint32_t addr_ptr    = memory.Read32(sp);
            uint32_t addrlen_ptr = memory.Read32(sp + 4);

            if (!buf_ptr) { 
                cpu->Regs()[0] = -1; 
                return; 
            }

            void* buf = memory.GetHostPointer(buf_ptr);
            struct sockaddr* src_addr = nullptr;
            socklen_t addrlen = 0;

            // recvfrom allows passing NULL if the caller doesn't care about the sender's address
            if (addr_ptr && addrlen_ptr) {
                src_addr = reinterpret_cast<struct sockaddr*>(memory.GetHostPointer(addr_ptr));
                addrlen = memory.Read32(addrlen_ptr);
            }

            ssize_t ret = ::recvfrom(sockfd, buf, len, flags, src_addr, (addr_ptr && addrlen_ptr) ? &addrlen : nullptr);

            // If it succeeded and the guest asked for the sender address, write the new size back
            if (ret >= 0 && addr_ptr && addrlen_ptr) {
                memory.Write32(addrlen_ptr, static_cast<uint32_t>(addrlen));
            }

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
            int sockfd          = static_cast<int>(cpu->Regs()[0]);
            int level           = static_cast<int>(cpu->Regs()[1]);
            int optname         = static_cast<int>(cpu->Regs()[2]);
            uint32_t optval_ptr = cpu->Regs()[3];
            
            // Argument 5 is passed on the stack
            uint32_t sp         = cpu->Regs()[13];
            uint32_t optlen     = memory.Read32(sp);
            
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

        ROUTE_REGISTER(router, "inet_ntoa", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t in_addr_val = cpu->Regs()[0]; // Passed by value
            
            struct in_addr addr;
            addr.s_addr = in_addr_val;
            
            char* str = ::inet_ntoa(addr);

            // Emulate the static buffer behavior of inet_ntoa
            static uint32_t guest_buf_ptr = 0;
            if (guest_buf_ptr == 0) {
                guest_buf_ptr = memory.AllocateHeap(16); // "255.255.255.255\0" max
            }
            
            if (str) {
                std::strncpy(reinterpret_cast<char*>(memory.GetHostPointer(guest_buf_ptr)), str, 16);
            } else {
                memory.Write8(guest_buf_ptr, 0);
            }
            
            cpu->Regs()[0] = guest_buf_ptr;
        });

    }
}


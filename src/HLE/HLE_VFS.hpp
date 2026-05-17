#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include <unistd.h>
#include <filesystem>
#include <sys/uio.h>
#include <fcntl.h>

namespace HLE::VFS {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        static std::unordered_map<uint32_t, FILE*> open_files;

        static std::mutex file_mutex;

        static uint32_t next_file_handle = 0x1000; // Start fake handles at 0x1000

        // Fake file storage
        static std::mutex fake_fd_mutex;
        static int next_fake_fd = 10000;               // start above real fds
        static std::unordered_map<int, std::string> fake_files; // fd -> content
        static std::unordered_map<int, size_t> fake_file_pos;  // fd -> current read position

        ROUTE_REGISTER(router, "fopen", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            uint32_t mode_ptr = cpu->Regs()[1];
            const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));
            const char* mode = reinterpret_cast<const char*>(memory.GetHostPointer(mode_ptr));

            std::lock_guard<std::mutex> lock(file_mutex);
            FILE* fp = std::fopen(path, mode);
            if (!fp) {
                cpu->Regs()[0] = 0;
                return;
            }

            int fd = ::fileno(fp);

            uint32_t guest_file = memory.AllocateHeap(256, cpu->Regs()[14]);
            uint16_t fd16 = static_cast<uint16_t>(fd);

            // Write fd as a short at every 2-byte aligned offset.
            // This covers Bionic's _file field regardless of whether it sits at
            // offset 0x0A, 0x0E, 0x12, etc.
            for (int i = 0; i < 256; i += 2) {
                memory.Write8(guest_file + i,     fd16 & 0xFF);
                memory.Write8(guest_file + i + 1, (fd16 >> 8) & 0xFF);
            }

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[VFS] fopen guest_file=0x" << std::hex << guest_file
                          << " fd=" << fd << " path=" << path << std::dec << std::endl;
            }

            open_files[guest_file] = fp;
            cpu->Regs()[0] = guest_file;
        });

        ROUTE_REGISTER(router, "fclose", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(file_mutex);
            uint32_t handle = cpu->Regs()[0];
            auto it = open_files.find(handle);
            if (it != open_files.end()) {
                std::fclose(it->second);
                open_files.erase(it);
                memory.FreeHeap(handle); // mimic Bionic freeing the FILE object
                cpu->Regs()[0] = 0;
            } else {
                cpu->Regs()[0] = EOF;
            }
        });

        ROUTE_REGISTER(router, "fread", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr   = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t nmemb = cpu->Regs()[2];
            uint32_t handle= cpu->Regs()[3];

            std::lock_guard<std::mutex> lock(file_mutex);

            if (open_files.find(handle) != open_files.end() && ptr) {
                uint64_t n = static_cast<uint64_t>(size) * nmemb;
                if (n > 0 && n <= 0xFFFFFFFFull) {
                    memory.CheckBoundedWrite(ptr, static_cast<uint32_t>(n), "fread", cpu->Regs()[14]);
                }
                void* host_ptr = memory.GetHostPointer(ptr);
                cpu->Regs()[0] = std::fread(host_ptr, size, nmemb, open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "fwrite", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t ptr   = cpu->Regs()[0];
            uint32_t size  = cpu->Regs()[1];
            uint32_t nmemb = cpu->Regs()[2];
            uint32_t handle= cpu->Regs()[3];

            std::lock_guard<std::mutex> lock(file_mutex);

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[VFS] fwrite handle=" << handle << " size=" << size << " nmemb=" << nmemb << std::endl;
            }
            
            if (open_files.find(handle) != open_files.end() && ptr) {
                void* host_ptr = memory.GetHostPointer(ptr);
                cpu->Regs()[0] = std::fwrite(host_ptr, size, nmemb, open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "fseek", [](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(file_mutex);
            uint32_t handle = cpu->Regs()[0];
            long int offset = static_cast<long int>(cpu->Regs()[1]);
            int whence = cpu->Regs()[2];

            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::fseek(open_files[handle], offset, whence);
            } else {
                cpu->Regs()[0] = -1; // Error
            }
        });

        ROUTE_REGISTER(router, "lseek", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd = static_cast<int>(cpu->Regs()[0]);
            off_t offset = static_cast<off_t>(cpu->Regs()[1]);
            int whence = cpu->Regs()[2];

            {
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                auto it = fake_files.find(fd);
                if (it != fake_files.end()) {
                    auto& content = it->second;
                    auto& pos = fake_file_pos[fd];
                    switch (whence) {
                        case SEEK_SET: pos = offset; break;
                        case SEEK_CUR: pos += offset; break;
                        case SEEK_END: pos = content.size() + offset; break;
                        default: cpu->Regs()[0] = -1; return;
                    }
                    // Clamp to valid range
                    if (pos > content.size()) pos = content.size();
                    cpu->Regs()[0] = static_cast<uint32_t>(pos);
                    std::cout << "[VFS] lseek fake fd=" << fd << " -> pos=" << pos << std::endl;
                    return;
                }
            }

            // Not a fake FD, use real lseek
            cpu->Regs()[0] = ::lseek(fd, offset, whence);
        });

        ROUTE_REGISTER(router, "ftell", [](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(file_mutex);
            uint32_t handle = cpu->Regs()[0];

            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::ftell(open_files[handle]);
            } else {
                cpu->Regs()[0] = -1; // Error
            }
        });

        ROUTE_REGISTER(router, "fflush", [](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(file_mutex);
            uint32_t handle = cpu->Regs()[0];
            if (open_files.find(handle) != open_files.end()) {
                cpu->Regs()[0] = std::fflush(open_files[handle]);
            } else {
                cpu->Regs()[0] = 0;
            }
        });

        ROUTE_REGISTER(router, "fputs", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];

            std::string str = reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr));
            std::cout << "[Guest stdout] " << str;
            cpu->Regs()[0] = 1; // Return >= 0 for success
        });

        ROUTE_REGISTER(router, "puts",[&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t str_ptr = cpu->Regs()[0];
            if (str_ptr) {
                std::cout << reinterpret_cast<const char*>(memory.GetHostPointer(str_ptr)) << "\n";
            }
            cpu->Regs()[0] = 1; // Success
        });

        ROUTE_REGISTER(router, "close", [](Dynarmic::A32::Jit* cpu) {
            int fd = static_cast<int>(cpu->Regs()[0]);
            {
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                if (fake_files.find(fd) != fake_files.end()) {
                    fake_files.erase(fd);
                    fake_file_pos.erase(fd);
                    cpu->Regs()[0] = 0;
                    return;
                }
            }
            cpu->Regs()[0] = ::close(fd);
        });

        ROUTE_REGISTER(router, "read", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd         = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf   = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            // --- Fake file support ---
            {
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                if (fake_files.find(fd) != fake_files.end()) {
                    std::string& content = fake_files[fd];
                    size_t& pos = fake_file_pos[fd];
                    size_t remaining = content.size() - pos;
                    size_t to_copy = std::min(static_cast<size_t>(count), remaining);
                    if (to_copy > 0) {
                        memory.CheckBoundedWrite(buf, static_cast<uint32_t>(to_copy), "read(fake)", cpu->Regs()[14]);
                    }
                    std::memcpy(memory.GetHostPointer(buf), content.data() + pos, to_copy);
                    pos += to_copy;
                    cpu->Regs()[0] = static_cast<uint32_t>(to_copy);
                    std::cout << "[VFS] read fake fd=" << fd << " returned " << to_copy << " bytes" << std::endl;
                    return;
                }
            }

            if (fd < 0) {
                cpu->Regs()[0] = 0; // stdin or invalid
                return;
            }

            if (count > 0) memory.CheckBoundedWrite(buf, count, "read", cpu->Regs()[14]);
            void* host_buf = memory.GetHostPointer(buf);
            ssize_t ret = ::read(fd, host_buf, count);
            cpu->Regs()[0] = static_cast<uint32_t>(ret);
        });

        ROUTE_REGISTER(router, "open", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            int flags = cpu->Regs()[1];
            int mode = cpu->Regs()[2];

            if (!path_ptr) { cpu->Regs()[0] = -1; return; }
            const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));

            // ----- Fake system files for Nexus 5 (2013) -----
            if (strcmp(path, "/proc/cpuinfo") == 0) {
                // nexus 5 /proc/cpuinfo
                std::string content =
                    "Processor\t: ARMv7 Processor rev 0 (v7l)\n"
                    "processor\t: 0\n"
                    "BogoMIPS\t: 38.40\n"
                    "\n"
                    "processor\t: 1\n"
                    "BogoMIPS\t: 38.40\n"
                    "\n"
                    "processor\t: 2\n"
                    "BogoMIPS\t: 38.40\n"
                    "\n"
                    "processor\t: 3\n"
                    "BogoMIPS\t: 38.40\n"
                    "\n"
                    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt \n"
                    "CPU implementer\t: 0x51\n"
                    "CPU architecture: 7\n"
                    "CPU variant\t: 0x2\n"
                    "CPU part\t: 0x06f\n"
                    "CPU revision\t: 0\n"
                    "\n"
                    "Hardware\t: Qualcomm MSM 8974 HAMMERHEAD (Flattened Device Tree)\n"
                    "Revision\t: 000b\n"
                    "Serial\t: 0000000000000000\n";
                    std::lock_guard<std::mutex> lock(fake_fd_mutex);
                    int fd = next_fake_fd++;
                    fake_files[fd] = content;
                    fake_file_pos[fd] = 0;
                    cpu->Regs()[0] = fd;
                    std::cout << "[VFS] open fake /proc/cpuinfo -> fd=" << fd << std::endl;
                    return;
            }
            if (strcmp(path, "/sys/devices/system/cpu/present") == 0) {
                std::string content = "0-3\n";
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                int fd = next_fake_fd++;
                fake_files[fd] = content;
                fake_file_pos[fd] = 0;
                cpu->Regs()[0] = fd;
                std::cout << "[VFS] open fake /proc/cpuinfo -> fd=" << fd << std::endl;
                return;
            }
            if (strcmp(path, "/sys/devices/system/cpu/possible") == 0) {
                std::string content = "0-3\n";
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                int fd = next_fake_fd++;
                fake_files[fd] = content;
                fake_file_pos[fd] = 0;
                cpu->Regs()[0] = fd;
                std::cout << "[VFS] open fake /proc/cpuinfo -> fd=" << fd << std::endl;
                return;
            }
            if (strcmp(path, "/proc/self/auxv") == 0) {
                // If the game still tries to read auxv after getauxval, return empty
                std::string content(8, '\0'); // 8 zero bytes = end of auxv
                std::lock_guard<std::mutex> lock(fake_fd_mutex);
                int fd = next_fake_fd++;
                fake_files[fd] = content;
                fake_file_pos[fd] = 0;
                cpu->Regs()[0] = fd;
                std::cout << "[VFS] open fake /proc/cpuinfo -> fd=" << fd << std::endl;
                return;
            }

            // Not a faked path – use real open
            cpu->Regs()[0] = ::open(path, flags, mode);
        });
        
        ROUTE_REGISTER(router, "write", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd = static_cast<int>(cpu->Regs()[0]);
            uint32_t buf_ptr = cpu->Regs()[1];
            uint32_t count = cpu->Regs()[2];

            uint32_t lr = cpu->Regs()[14];

            {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[VFS] write caller LR=0x" << std::hex << lr << std::dec
                          << " fd=" << fd << " count=" << count << std::endl;
            }

            if (fd == 1 || fd == 2) {
                cpu->Regs()[0] = count; // Swallow guest stdout/stderr
                return;
            }
            const void* buf = memory.GetHostPointer(buf_ptr);
            cpu->Regs()[0] = ::write(fd, buf, count);
        });

        ROUTE_REGISTER(router, "scandir", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t dirp_ptr     = cpu->Regs()[0];
            uint32_t namelist_ptr = cpu->Regs()[1];

            // Read guest directory path
            const char* guest_dir_ptr =
                reinterpret_cast<const char*>(memory.GetHostPointer(dirp_ptr));

            if (!guest_dir_ptr) {
                cpu->Regs()[0] = -1;
                return;
            }

            std::string host_dir = guest_dir_ptr;

            std::vector<std::string> found_files;

            try {
                if (std::filesystem::exists(host_dir) &&
                    std::filesystem::is_directory(host_dir)) {

                    for (const auto& entry :
                         std::filesystem::directory_iterator(host_dir)) {

                        found_files.push_back(
                            entry.path().filename().string()
                        );
                    }
                }
            }
            catch (const std::exception&) {
                cpu->Regs()[0] = -1;
                return;
            }

            if (found_files.empty()) {
                cpu->Regs()[0] = 0;
                return;
            }

            // Allocate array of guest pointers (dirent**)
            uint32_t array_ptr =
                memory.AllocateHeap(found_files.size() * sizeof(uint32_t));

            memory.Write32(namelist_ptr, array_ptr);

            // Android/Bionic dirent layout:
            //
            // uint64_t d_ino
            // int64_t  d_off
            // uint16_t d_reclen
            // uint8_t  d_type
            // char     d_name[256]
            //
            // d_name offset = 19

            constexpr uint32_t DIRENT_SIZE  = 280;
            constexpr uint32_t D_NAME_OFFSET = 19;
            constexpr uint32_t D_NAME_MAX    = 255;

            for (size_t i = 0; i < found_files.size(); i++) {
                uint32_t dirent_ptr =
                    memory.AllocateHeap(DIRENT_SIZE);

                // Store pointer into namelist array
                memory.Write32(
                    array_ptr + (i * sizeof(uint32_t)),
                    dirent_ptr
                );

                char* name_ptr =
                    reinterpret_cast<char*>(
                        memory.GetHostPointer(
                            dirent_ptr + D_NAME_OFFSET
                        )
                    );

                if (!name_ptr)
                    continue;

                std::strncpy(
                    name_ptr,
                    found_files[i].c_str(),
                    D_NAME_MAX
                );

                name_ptr[D_NAME_MAX] = '\0';
            }

            cpu->Regs()[0] = found_files.size();
        });

        ROUTE_REGISTER(router, "unlink", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            if (path_ptr) {
                const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));
                cpu->Regs()[0] = unlink(path);
            } else {
                cpu->Regs()[0] = -1;
            }
        });

        ROUTE_REGISTER(router, "writev", [&memory](Dynarmic::A32::Jit* cpu) {
            int fd = static_cast<int>(cpu->Regs()[0]);
            uint32_t iov_ptr = cpu->Regs()[1];
            int iovcnt = cpu->Regs()[2];

            std::vector<struct iovec> host_iov(iovcnt);
            size_t total_len = 0;
            for (int i = 0; i < iovcnt; ++i) {
                uint32_t base = memory.Read32(iov_ptr + (i * 8));
                uint32_t len  = memory.Read32(iov_ptr + (i * 8) + 4);
                host_iov[i].iov_base = base ? memory.GetHostPointer(base) : nullptr;
                host_iov[i].iov_len  = len;
                total_len += len;
            }

            if (fd == 1 || fd == 2) {
                cpu->Regs()[0] = total_len; // Swallow guest stdout/stderr!
                return;
            }
            cpu->Regs()[0] = ::writev(fd, host_iov.data(), iovcnt);
        });

        ROUTE_REGISTER(router, "select", [&memory](Dynarmic::A32::Jit* cpu) {
            int nfds = cpu->Regs()[0];
            uint32_t readfds_ptr = cpu->Regs()[1];
            uint32_t writefds_ptr = cpu->Regs()[2];
            uint32_t exceptfds_ptr = cpu->Regs()[3];
            uint32_t timeout_ptr = memory.Read32(cpu->Regs()[13]); // Arg 5 on stack

            // Luckily, fd_set is 128 bytes on both 32-bit Android and 64-bit Linux
            fd_set* h_read = readfds_ptr ? (fd_set*)memory.GetHostPointer(readfds_ptr) : nullptr;
            fd_set* h_write = writefds_ptr ? (fd_set*)memory.GetHostPointer(writefds_ptr) : nullptr;
            fd_set* h_except = exceptfds_ptr ? (fd_set*)memory.GetHostPointer(exceptfds_ptr) : nullptr;

            // But timeval is different (8 bytes guest vs 16 bytes host)
            struct timeval tv;
            struct timeval* p_tv = nullptr;
            if (timeout_ptr) {
                tv.tv_sec = memory.Read32(timeout_ptr);
                tv.tv_usec = memory.Read32(timeout_ptr + 4);
                p_tv = &tv;
            }

            cpu->Regs()[0] = ::select(nfds, h_read, h_write, h_except, p_tv);
        });

        ROUTE_REGISTER(router, "access", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t path_ptr = cpu->Regs()[0];
            int mode = cpu->Regs()[1];

            const char* path = reinterpret_cast<const char*>(memory.GetHostPointer(path_ptr));
            cpu->Regs()[0] = ::access(path, mode);
        });

    }
}

#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "Config.hpp"

#include <zlib.h>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <utility>

namespace HLE::Zlib {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        constexpr uint32_t Z32_NEXT_IN   = 0;
        constexpr uint32_t Z32_AVAIL_IN  = 4;
        constexpr uint32_t Z32_TOTAL_IN  = 8;
        constexpr uint32_t Z32_NEXT_OUT  = 12;
        constexpr uint32_t Z32_AVAIL_OUT = 16;
        constexpr uint32_t Z32_TOTAL_OUT = 20;
        constexpr uint32_t Z32_ADLER     = 48;

        static std::mutex zlib_mutex;
        static std::unordered_map<uint32_t, std::unique_ptr<z_stream>> host_zstreams;

        auto z_sync_in =[](GuestMemory& mem, uint32_t gp, z_stream& zs, uint32_t& old_next_in, uint32_t& old_avail_in, uint32_t& old_next_out, uint32_t& old_avail_out) {
            old_next_in   = mem.Read32(gp + Z32_NEXT_IN);
            old_avail_in  = mem.Read32(gp + Z32_AVAIL_IN);
            old_next_out  = mem.Read32(gp + Z32_NEXT_OUT);
            old_avail_out = mem.Read32(gp + Z32_AVAIL_OUT);

            zs.avail_in  = old_avail_in;
            zs.avail_out = old_avail_out;
            zs.next_in   = old_next_in  ? mem.GetHostPointer(old_next_in)  : nullptr;
            zs.next_out  = old_next_out ? mem.GetHostPointer(old_next_out) : nullptr;
        };

        auto z_sync_out =[](GuestMemory& mem, uint32_t gp, z_stream& zs, uint32_t old_next_in, uint32_t old_avail_in, uint32_t old_next_out, uint32_t old_avail_out) {
            uint32_t consumed  = old_avail_in  - zs.avail_in;
            uint32_t produced  = old_avail_out - zs.avail_out;

            mem.Write32(gp + Z32_NEXT_IN,   old_next_in  + consumed);
            mem.Write32(gp + Z32_AVAIL_IN,  zs.avail_in);
            mem.Write32(gp + Z32_NEXT_OUT,  old_next_out + produced);
            mem.Write32(gp + Z32_AVAIL_OUT, zs.avail_out);
            mem.Write32(gp + Z32_TOTAL_IN,  static_cast<uint32_t>(zs.total_in));
            mem.Write32(gp + Z32_TOTAL_OUT, static_cast<uint32_t>(zs.total_out));
            mem.Write32(gp + Z32_ADLER,     static_cast<uint32_t>(zs.adler));
        };

        ROUTE_REGISTER(router, "inflate", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(zlib_mutex);
            uint32_t stream_ptr = cpu->Regs()[0];
            int flush = static_cast<int>(cpu->Regs()[1]);

            if (host_zstreams.find(stream_ptr) == host_zstreams.end()) {
                std::cout << "[ZLIB] ERROR: inflate on unknown stream 0x" << std::hex << stream_ptr << std::dec << "\n";
                cpu->Regs()[0] = Z_STREAM_ERROR;
                return;
            }

            auto& zs = host_zstreams[stream_ptr];
    
            // Sync IN from Guest to Host
            uint32_t oni = memory.Read32(stream_ptr + Z32_NEXT_IN);
            uint32_t oai = memory.Read32(stream_ptr + Z32_AVAIL_IN);
            uint32_t ono = memory.Read32(stream_ptr + Z32_NEXT_OUT);
            uint32_t oao = memory.Read32(stream_ptr + Z32_AVAIL_OUT);

            zs->avail_in  = oai;
            zs->avail_out = oao;
            zs->next_in   = oni ? memory.GetHostPointer(oni) : nullptr;
            zs->next_out  = ono ? memory.GetHostPointer(ono) : nullptr;

            // Perform Host Decompression
            int ret = ::inflate(zs.get(), flush);

            // write host's 'msg' translation back
            memory.Write32(stream_ptr + 24, 0); // msg = NULL on success

            // Sync OUT from Host to Guest
            uint32_t consumed = oai - zs->avail_in;
            uint32_t produced = oao - zs->avail_out;

            memory.Write32(stream_ptr + Z32_NEXT_IN,   oni + consumed);
            memory.Write32(stream_ptr + Z32_AVAIL_IN,  zs->avail_in);
            memory.Write32(stream_ptr + Z32_NEXT_OUT,  ono + produced);
            memory.Write32(stream_ptr + Z32_AVAIL_OUT, zs->avail_out);
            memory.Write32(stream_ptr + Z32_TOTAL_IN,  static_cast<uint32_t>(zs->total_in));
            memory.Write32(stream_ptr + Z32_TOTAL_OUT, static_cast<uint32_t>(zs->total_out));
            memory.Write32(stream_ptr + Z32_ADLER,     static_cast<uint32_t>(zs->adler));

            if constexpr (Config::Prints::zlib) {
                std::lock_guard<std::mutex> lock(console_mutex);
                std::cout << "[zlib] inflate(flush=" << flush << ") | "
                          << "IN: " << oai << " (used " << consumed << ") | "
                          << "OUT: " << oao << " (made " << produced << ") | "
                          << "RET: " << ret << std::endl;
            }

            // Print error if something went wrong (Z_BUF_ERROR is fine, just means it needs more data)
            if (ret < 0 && ret != Z_BUF_ERROR) {
                std::cout << "[ZLIB] inflate ERROR code: " << ret << " (msg: " << (zs->msg ? zs->msg : "none") << ")\n";
            }

            if (ret == Z_OK) {
                // write host's 'msg' translation back
                memory.Write32(stream_ptr + 24, 0); // msg = NULL on success
            }

            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "inflateEnd",[](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(zlib_mutex);
            uint32_t stream_ptr = cpu->Regs()[0];
            if (host_zstreams.find(stream_ptr) != host_zstreams.end()) {
                inflateEnd(host_zstreams[stream_ptr].get());
                host_zstreams.erase(stream_ptr);
                cpu->Regs()[0] = Z_OK;
            } else {
                cpu->Regs()[0] = Z_STREAM_ERROR;
            }
        });

        ROUTE_REGISTER(router, "inflateReset",[](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(zlib_mutex);
            uint32_t stream_ptr = cpu->Regs()[0];
            if (host_zstreams.find(stream_ptr) != host_zstreams.end()) {
                cpu->Regs()[0] = inflateReset(host_zstreams[stream_ptr].get());
            } else {
                cpu->Regs()[0] = Z_STREAM_ERROR;
            }
        });

        ROUTE_REGISTER(router, "inflateInit_", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(zlib_mutex);
            uint32_t stream_ptr = cpu->Regs()[0];
            
            // Create a heap-allocated unique_ptr
            auto zs = std::make_unique<z_stream>();
            zs->zalloc = Z_NULL;
            zs->zfree = Z_NULL;
            zs->opaque = Z_NULL;

            // Initialize it (passing the raw pointer using .get())
            int ret = inflateInit(zs.get());
            
            if (ret == Z_OK) {
                // MOVE ownership of the smart pointer into the map
                host_zstreams[stream_ptr] = std::move(zs);
                memory.Write32(stream_ptr + 24, 0);  // msg
                memory.Write32(stream_ptr + 28, 0);  // state (opaque to guest)
                memory.Write32(stream_ptr + 44, 0);  // data_type
            }
            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "inflateInit2_", [&memory](Dynarmic::A32::Jit* cpu) {
            std::lock_guard<std::mutex> lock(zlib_mutex);
            uint32_t stream_ptr = cpu->Regs()[0];
            int windowBits = static_cast<int>(cpu->Regs()[1]);

            // Create a heap-allocated unique_ptr
            auto zs = std::make_unique<z_stream>();
            zs->zalloc = Z_NULL;
            zs->zfree = Z_NULL;
            zs->opaque = Z_NULL;

            // Initialize
            int ret = inflateInit2(zs.get(), windowBits);
            
            if (ret == Z_OK) {
                // MOVE ownership of the smart pointer into the map
                host_zstreams[stream_ptr] = std::move(zs);
                memory.Write32(stream_ptr + 24, 0);  // msg
                memory.Write32(stream_ptr + 28, 0);  // state (opaque to guest)
                memory.Write32(stream_ptr + 44, 0);  // data_type
            }
            cpu->Regs()[0] = ret;
        });

        ROUTE_REGISTER(router, "crc32", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t crc_in = cpu->Regs()[0];
            uint32_t buf_ptr = cpu->Regs()[1];
            uint32_t len = cpu->Regs()[2];
            if (buf_ptr == 0) cpu->Regs()[0] = crc32(crc_in, Z_NULL, 0);
            else cpu->Regs()[0] = crc32(crc_in, memory.GetHostPointer(buf_ptr), len);
        });

    }
}

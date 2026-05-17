#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Range-based watchpoint registry used by the diagnostic instrumentation.
//
// HLE_Threading registers each guest pthread's stack via Add() and removes
// it on pthread_exit. EmuCallbacks calls Find() on every guest write to
// detect cross-thread writes into another thread's live stack. GuestMemory
// also calls Find() after picking a free block in AllocateHeap to detect
// allocations that overlap a still-live thread stack.
struct Watchpoint {
    struct Range {
        uint32_t lo;
        uint32_t hi; // exclusive
        std::thread::id owner;
        const char* tag;
    };

    inline static std::vector<Range> ranges;
    inline static std::mutex mu;
    // Fast-path skip when no ranges are registered.
    inline static std::atomic<int> active_count{0};

    static void Add(uint32_t lo, uint32_t hi, const char* tag) {
        std::lock_guard<std::mutex> lock(mu);
        ranges.push_back({lo, hi, std::this_thread::get_id(), tag});
        active_count.fetch_add(1, std::memory_order_relaxed);
    }

    static void Remove(uint32_t lo) {
        std::lock_guard<std::mutex> lock(mu);
        for (auto it = ranges.begin(); it != ranges.end(); ++it) {
            if (it->lo == lo) {
                ranges.erase(it);
                active_count.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // Returns the first range overlapping [addr, addr+size).
    static std::optional<Range> Find(uint32_t addr, uint32_t size) {
        if (active_count.load(std::memory_order_relaxed) == 0) return std::nullopt;
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& r : ranges) {
            if (addr < r.hi && (addr + size) > r.lo) return r;
        }
        return std::nullopt;
    }

    static std::vector<Range> Snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return ranges;
    }

    static std::string TidString(std::thread::id id) {
        std::ostringstream os;
        os << id;
        return os.str();
    }
};

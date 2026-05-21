#pragma once
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "ElfLoader.hpp"
#include "EmuCallbacks.hpp"
#include "AndroidCP15.hpp"
#include "ThreadingHelpers.hpp"
#include "CPUHelper.hpp"
#include "Watchpoint.hpp"
#include "Config.hpp"

#include <dynarmic/interface/exclusive_monitor.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace HLE::Threading {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory, ElfLoader& loader, Dynarmic::ExclusiveMonitor& monitor) {

        static std::mutex host_mutex_lock;
        static std::unordered_map<uint32_t, std::shared_ptr<std::recursive_mutex>> host_mutexes;

        // Condition variables live in a parallel map. condition_variable_any
        // (not the regular one) so it pairs with the recursive_mutex above.
        static std::mutex host_cv_lock;
        static std::unordered_map<uint32_t, std::shared_ptr<std::condition_variable_any>> host_cvs;

        auto pthread_success = [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = 0; // 0 = Success
        };

        static std::mutex once_global_mutex;   // serialises all pthread_once calls

        ROUTE_REGISTER(router, "pthread_attr_setschedparam", pthread_success);
        ROUTE_REGISTER(router, "pthread_attr_setstacksize", pthread_success);
        ROUTE_REGISTER(router, "pthread_attr_setdetachstate", pthread_success);

        ROUTE_REGISTER(router, "pthread_mutexattr_init", pthread_success);
        ROUTE_REGISTER(router, "pthread_mutexattr_settype", pthread_success);
        ROUTE_REGISTER(router, "pthread_mutexattr_destroy", pthread_success);

        ROUTE_REGISTER(router, "pthread_mutex_init", pthread_success);
        ROUTE_REGISTER(router, "pthread_cond_init", pthread_success);

        ROUTE_REGISTER(router, "pthread_key_create", pthread_success);
        ROUTE_REGISTER(router, "pthread_setname_np", pthread_success);


        ROUTE_REGISTER(router, "pthread_create", [&memory, &loader, &router, &monitor](Dynarmic::A32::Jit* cpu) {
            uint32_t thread_ptr = cpu->Regs()[0];
            uint32_t entry = cpu->Regs()[2];
            uint32_t arg = cpu->Regs()[3];
            
            // Allocate a unique Stack and TLS area for the new thread.
            // Pass the guest LR as caller_pc so the AllocDump can identify the spawn site.
            uint32_t stack_size = 1024 * 1024;
            uint32_t stack_base = memory.AllocateHeap(stack_size, cpu->Regs()[14]);
            uint32_t sp = stack_base + stack_size;
            uint32_t tls = memory.AllocateHeap(4096, cpu->Regs()[14]);

            static std::atomic<int> next_core_id{1};
            int core_id = next_core_id.fetch_add(1);

            if (thread_ptr) memory.Write32(thread_ptr, core_id);

            // Fire off the background thread
            std::thread([&memory, &loader, &router, &monitor, entry, arg, sp, stack_base, tls, core_id, stack_size]() {

                // Register this thread's stack range so any cross-thread write
                // into it (or any AllocateHeap that returns an overlapping block)
                // gets caught immediately. Owner is *this* host thread.
                Watchpoint::Add(stack_base, stack_base + stack_size, "worker_stack");

                // Each thread gets its own callback handler pointing to its own CPU
                EmuCallbacks callbacks(memory, loader, router);

                Dynarmic::A32::UserConfig config;
                config.callbacks = &callbacks;
                config.page_table = &memory.page_table;
                config.absolute_offset_page_table = false;
                // Fastmem on workers: matched to the main thread via Config.
                // Off keeps cross-thread Watchpoint checks live; on is the
                // perf default.
                config.fastmem_pointer = Config::Performance::fastmem
                    ? reinterpret_cast<uintptr_t>(memory.fastmem_base) : 0;
                config.recompile_on_fastmem_failure = true;
                config.arch_version = Dynarmic::A32::ArchVersion::v7;

                // Share the global monitor, give thread a unique ID and its own CP15/TLS
                config.global_monitor = &monitor;
                config.processor_id = core_id;
                config.coprocessors[15] = std::make_shared<AndroidCP15>(tls);

                Dynarmic::A32::Jit thread_cpu(config);
                callbacks.cpu = &thread_cpu;
                // change active CPU and thread
                active_cpu = &thread_cpu;
                active_thread_id = core_id;

                // Setup initial Registers
                thread_cpu.Regs()[13] = sp;
                thread_cpu.Regs()[0] = arg;
                thread_cpu.Regs()[14] = loader.GetThunk("pthread_exit");
                thread_cpu.Regs()[15] = entry & ~1;
                thread_cpu.SetCpsr((entry & 1) ? 0x30 : 0x10);

                // Background Emulator Loop
                while (true) {
                    auto halt = thread_cpu.Run();

                    // Check if the CPU halted because it reached our exit SVC
                    if (halt == Dynarmic::HaltReason::UserDefined1 &&
                        thread_cpu.Regs()[15] == 0xFFFFFFFE) {
                        break; // Clean exit
                    }

                    if (halt == Dynarmic::HaltReason::UserDefined1) {
                        // Some other UserDefined1 (e.g. Emulator_Return_Trap fired
                        // from a nested call site). Clear and continue so we don't
                        // leave the halt latched.
                        thread_cpu.ClearHalt(Dynarmic::HaltReason::UserDefined1);
                    }

                    if (halt == Dynarmic::HaltReason::UserDefined2) {
                        thread_cpu.ClearHalt(Dynarmic::HaltReason::UserDefined2);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }

                    if (halt == Dynarmic::HaltReason::UserDefined3) {
                        thread_cpu.ClearHalt(Dynarmic::HaltReason::UserDefined3);
                        if (once_saved_state.valid) {
                            for (int r = 0; r < 16; r++)
                                thread_cpu.Regs()[r] = once_saved_state.regs[r];
                            thread_cpu.SetCpsr(once_saved_state.cpsr);
                            once_saved_state.valid = false;
                        }
                    }

                    // Any other halt reason (including halt_reason=0, which just
                    // means Dynarmic ran out of its tick budget from
                    // GetTicksRemaining()) is implicitly handled by looping back
                    // around and re-entering Run().
                }

                // Unregister stack range BEFORE the FreeHeap so a racing AllocateHeap
                // on another thread can't be flagged as overlapping a stack that is
                // already on its way out.
                Watchpoint::Remove(stack_base);

                // Clean up memory after thread exit
                memory.FreeHeap(stack_base);
                memory.FreeHeap(tls);
                std::cout << "[Threading] Background thread exited gracefully." << std::endl;

            }).detach(); // Detach allows it to run freely alongside the main threads
            
            cpu->Regs()[0] = 0; // Success
        });

        ROUTE_REGISTER(router, "pthread_exit",[](Dynarmic::A32::Jit* cpu) {
            // Set a magic PC so the host loop knows to terminate this thread cleanly
            cpu->Regs()[15] = 0xFFFFFFFE;
            // Safely break out of the CPU loop.
            cpu->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        });

        ROUTE_REGISTER(router, "pthread_self", [](Dynarmic::A32::Jit* cpu) {
            cpu->Regs()[0] = active_thread_id; // Return our thread_local created id
        });

        ROUTE_REGISTER(router, "pthread_once", [&](Dynarmic::A32::Jit* cpu) {
            uint32_t once_control_ptr = cpu->Regs()[0];
            uint32_t init_routine     = cpu->Regs()[1];

            // Use unique_lock so we can unlock manually
            std::unique_lock<std::mutex> lock(once_global_mutex);

            int current_val = once_control_ptr ? static_cast<int>(memory.Read32(once_control_ptr)) : 2;
            if (current_val == 2) {
                cpu->Regs()[0] = 0;   // already done
                return;
            }

            // Mark as done BEFORE releasing the lock – other threads *should* immediately see 2 ...
            if (once_control_ptr)
                memory.Write32(once_control_ptr, 2);

            // Save the complete CPU context (thread‑local, shared with the loop)
            for (int i = 0; i < 16; i++)
                once_saved_state.regs[i] = cpu->Regs()[i];
            once_saved_state.cpsr = cpu->Cpsr();
            once_saved_state.valid = true;

            // Prepare the CPU to run the init routine
            cpu->Regs()[14] = loader.GetThunk("pthread_once_done");   // return address
            cpu->Regs()[15] = init_routine & ~1;                      // start of init
            uint32_t new_cpsr = (init_routine & 1) ? 0x00000030 : 0x00000010;
            cpu->SetCpsr(new_cpsr);

            lock.unlock();

            // syscall returns normally (R0=0), and Dynarmic should execute the init routine
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_attr_init", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t attr = cpu->Regs()[0];
            if (attr) {
                for (int i = 0; i < 24; i++) memory.Write8(attr + i, 0);
            }
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_mutex_lock",[](Dynarmic::A32::Jit* cpu) {
            uint32_t mutex_ptr = cpu->Regs()[0];
            std::shared_ptr<std::recursive_mutex> m;
            {
                std::lock_guard<std::mutex> lock(host_mutex_lock);
                if (!host_mutexes[mutex_ptr]) host_mutexes[mutex_ptr] = std::make_shared<std::recursive_mutex>();
                m = host_mutexes[mutex_ptr];
            }
            m->lock(); // Block natively
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_mutex_unlock",[](Dynarmic::A32::Jit* cpu) {
            uint32_t mutex_ptr = cpu->Regs()[0];
            std::shared_ptr<std::recursive_mutex> m;
            {
                std::lock_guard<std::mutex> lock(host_mutex_lock);
                m = host_mutexes[mutex_ptr];
            }
            if (m) m->unlock();
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_mutex_destroy",[](Dynarmic::A32::Jit* cpu) {
            uint32_t mutex_ptr = cpu->Regs()[0];
            std::lock_guard<std::mutex> lock(host_mutex_lock);
            host_mutexes.erase(mutex_ptr);
            cpu->Regs()[0] = 0;
        });

        // --- Condition variables ---
        // All five are needed for the producer/consumer pattern Octarine
        // uses for asset loading and the character-list scanner. Without
        // proper signaling, timedwait used to return ETIMEDOUT immediately
        // and the caller's "wait for worker" loop became a busy poll;
        // worse, if the main thread held a mutex the worker needed, the
        // missing signal turned the poll into a hang.

        auto get_cv = [](uint32_t cv_ptr) -> std::shared_ptr<std::condition_variable_any> {
            std::lock_guard<std::mutex> lock(host_cv_lock);
            auto& slot = host_cvs[cv_ptr];
            if (!slot) slot = std::make_shared<std::condition_variable_any>();
            return slot;
        };

        auto get_mutex = [](uint32_t mutex_ptr) -> std::shared_ptr<std::recursive_mutex> {
            std::lock_guard<std::mutex> lock(host_mutex_lock);
            auto& slot = host_mutexes[mutex_ptr];
            if (!slot) slot = std::make_shared<std::recursive_mutex>();
            return slot;
        };

        ROUTE_REGISTER(router, "pthread_cond_destroy", [](Dynarmic::A32::Jit* cpu) {
            uint32_t cv_ptr = cpu->Regs()[0];
            std::lock_guard<std::mutex> lock(host_cv_lock);
            host_cvs.erase(cv_ptr);
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_cond_signal", [get_cv](Dynarmic::A32::Jit* cpu) {
            auto cv = get_cv(cpu->Regs()[0]);
            cv->notify_one();
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_cond_broadcast", [get_cv](Dynarmic::A32::Jit* cpu) {
            auto cv = get_cv(cpu->Regs()[0]);
            cv->notify_all();
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_cond_wait", [get_cv, get_mutex](Dynarmic::A32::Jit* cpu) {
            auto cv = get_cv(cpu->Regs()[0]);
            auto m  = get_mutex(cpu->Regs()[1]);
            // The guest already holds the mutex (pthread_mutex_lock was
            // called before this). adopt_lock + release at the end keeps
            // it held across the wait/wake cycle, which is the contract.
            std::unique_lock<std::recursive_mutex> lock(*m, std::adopt_lock);
            cv->wait(lock);
            lock.release();
            cpu->Regs()[0] = 0;
        });

        ROUTE_REGISTER(router, "pthread_cond_timedwait", [&memory, get_cv, get_mutex](Dynarmic::A32::Jit* cpu) {
            uint32_t cv_ptr     = cpu->Regs()[0];
            uint32_t mutex_ptr  = cpu->Regs()[1];
            uint32_t abstime_ptr = cpu->Regs()[2];

            auto cv = get_cv(cv_ptr);
            auto m  = get_mutex(mutex_ptr);

            // Bionic abstime is CLOCK_REALTIME-based (struct timespec).
            // Convert to a system_clock time_point for wait_until.
            std::chrono::time_point<std::chrono::system_clock> deadline;
            if (abstime_ptr) {
                int64_t tv_sec  = static_cast<int32_t>(memory.Read32(abstime_ptr));
                int64_t tv_nsec = static_cast<int32_t>(memory.Read32(abstime_ptr + 4));
                deadline = std::chrono::system_clock::time_point{}
                         + std::chrono::seconds(tv_sec)
                         + std::chrono::nanoseconds(tv_nsec);
            } else {
                deadline = std::chrono::system_clock::now();
            }

            std::unique_lock<std::recursive_mutex> lock(*m, std::adopt_lock);
            auto status = cv->wait_until(lock, deadline);
            lock.release();

            cpu->Regs()[0] = (status == std::cv_status::timeout) ? 110 /* ETIMEDOUT */ : 0;
        });

    }
}

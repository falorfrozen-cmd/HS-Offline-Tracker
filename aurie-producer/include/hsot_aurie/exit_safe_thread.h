#pragma once

#include <Windows.h>

#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace hsot::aurie {

// How long an unload waits for the publisher to end before it pins the module
// instead (see StopForUnload). The publisher wakes every 16 ms, so on an
// ordinary thread it ends within one cycle.
inline constexpr std::chrono::milliseconds kUnloadJoinTimeout{2000};

// A background thread owned by a module global, with no destructor.
//
// The game ends with ExitProcess, which terminates every other thread first and
// then runs each DLL's DLL_PROCESS_DETACH, where the CRT destroys this module's
// static objects. A std::thread whose thread was killed that way is still
// joinable, and destroying a joinable std::thread calls std::terminate: the
// ucrtbase abort (0xc0000409, fast-fail 7) that game exits used to report.
// Nothing can join first: Aurie runs no unload routine at process exit
// (AurieCore's DllMain returns at once when the process is ending), this module
// cannot have a DllMain of its own (Aurie's shared.hpp defines it), and a join
// in DllMain would wait for a thread that cannot exit while the loader lock is
// held.
//
// So the std::thread lives on the heap and is freed only after a successful
// join. At process exit nothing runs, and the OS reclaims the rest.
class ExitSafeThread final {
public:
    // Starts `routine` on a new thread. False when this object already owns a
    // thread or the thread could not be created.
    [[nodiscard]] bool Start(void (*routine)() noexcept) noexcept {
        if (thread_) {
            return false;
        }
        try {
            thread_ = new std::thread(routine);
            return true;
        } catch (...) {
            return false;
        }
    }

    // Waits up to `timeout` for the thread, which the caller has already asked
    // to stop, then joins and frees it. True when no thread is left (joined
    // here, or never started). False when it is still running: it is left
    // alone, because a joinable std::thread must never be destroyed, and the
    // caller must keep this module mapped.
    [[nodiscard]] bool JoinFor(const std::chrono::milliseconds timeout) noexcept {
        if (!thread_) {
            return true;
        }
        if (WaitForSingleObject(thread_->native_handle(), static_cast<DWORD>(timeout.count())) !=
            WAIT_OBJECT_0) {
            return false;
        }
        try {
            thread_->join();
        } catch (...) {
            return false;
        }
        delete std::exchange(thread_, nullptr);
        return true;
    }

private:
    std::thread* thread_{};
};

static_assert(std::is_trivially_destructible_v<ExitSafeThread>,
    "a module global must not end a thread in its destructor; see ExitSafeThread");

// Keeps the module that contains this code loaded until the process exits, so a
// later FreeLibrary no longer unmaps it. Callable with the loader lock held.
[[nodiscard]] inline bool PinThisModule() noexcept {
    HMODULE module{};
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&__ImageBase),
               &module) != FALSE;
}

enum class UnloadOutcome {
    // The publisher was joined and `stop_transport` ran.
    stopped,
    // The publisher did not end in time. The module stays loaded, with the
    // threads it still owns, until the process exits.
    pinned,
    // As `pinned`, but pinning failed: FreeLibrary will unmap code those
    // threads may still run.
    pin_failed,
};

// The stop sequence for Aurie's ModuleUnload, which Aurie calls from
// MdUnmapImage on an ordinary thread and also from its own DLL_PROCESS_DETACH
// when the framework itself is unloaded (the Aurie console's "Unload
// framework"). In the second case the loader lock is held, no thread can finish
// exiting, and an unbounded join never returns.
//
// The publisher, already told to stop, is therefore joined with a bound. If it
// ended, threads can exit in this context, so `stop_transport` may join the
// pipe worker. If it did not, the module is pinned instead, so FreeLibrary
// cannot unmap code the threads still run.
template <class StopTransport>
[[nodiscard]] UnloadOutcome StopForUnload(
    ExitSafeThread& publisher,
    StopTransport&& stop_transport) noexcept {
    if (!publisher.JoinFor(kUnloadJoinTimeout)) {
        return PinThisModule() ? UnloadOutcome::pinned : UnloadOutcome::pin_failed;
    }
    stop_transport();
    return UnloadOutcome::stopped;
}

} // namespace hsot::aurie

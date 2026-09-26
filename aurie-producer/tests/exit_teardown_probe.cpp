// A stand-in for HSOfflineTrackerProducer.dll's threads, loaded by
// exit_teardown_smoke.cpp. It runs the producer's real pipe transport and a
// publisher shaped like PublishWorkerLoop, owned in one of these shapes:
//
//   kShapeCurrent: as src/module.cpp owns them (an ExitSafeThread and a plain
//                  transport pointer), stopped by ProbeUnload the way
//                  ModuleUnload stops them;
//   kShapeStalled: the same, with a publisher that takes 3 s to end once told
//                  to stop, so an unload has to pin the module;
//   kShapeBeforeFix: as module.cpp owned them before ExitSafeThread (a
//                  std::unique_ptr and a std::thread with static storage, in
//                  that declaration order).
//
// No game, Aurie or YYToolkit is involved: what is under test is how a DLL's
// threads and globals meet ExitProcess and FreeLibrary.

#include "hsot/event_transport.h"
#include "hsot_aurie/exit_safe_thread.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

constexpr int kShapeCurrent = 0;
constexpr int kShapeStalled = 1;
constexpr int kShapeBeforeFix = 2;

std::mutex g_transport_mutex;
std::atomic_bool g_stopping{};
std::atomic_bool g_transport_stopped{};
std::mutex g_publish_wait_mutex;
std::condition_variable g_publish_wake;
int g_shape{kShapeCurrent};
std::atomic_int g_last_unload{-1};

hsot::IEventTransport* g_transport{};
hsot::aurie::ExitSafeThread g_publish_worker;

std::unique_ptr<hsot::IEventTransport> g_before_fix_transport;
std::thread g_before_fix_publish_worker;

[[nodiscard]] hsot::IEventTransport* Transport() noexcept {
    return g_shape == kShapeBeforeFix ? g_before_fix_transport.get() : g_transport;
}

void PublishWorkerLoop() noexcept {
    static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
    std::unique_lock wait_lock(g_publish_wait_mutex);
    std::uint64_t cycle{};
    while (!g_stopping.load(std::memory_order_acquire)) {
        g_publish_wake.wait_for(wait_lock, std::chrono::milliseconds(16));
        if (g_stopping.load(std::memory_order_acquire)) {
            break;
        }
        wait_lock.unlock();
        try {
            const std::string line =
                "{\"protocol\":\"hs-offline-tracker/1\",\"v\":1,\"kind\":\"heartbeat\",\"cycle\":" +
                std::to_string(++cycle) + "}\n";
            std::scoped_lock lock(g_transport_mutex);
            if (auto* transport = Transport()) {
                static_cast<void>(transport->TryPublish(line));
            }
        } catch (...) {
        }
        wait_lock.lock();
    }
    if (g_shape == kShapeStalled) {
        wait_lock.unlock();
        Sleep(3000);
    }
}

void StopTransport() noexcept {
    g_stopping.store(true, std::memory_order_release);
    if (g_transport_stopped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::scoped_lock lock(g_transport_mutex);
    if (g_transport) {
        g_transport->Stop();
    }
}

void ReleaseTransport() noexcept {
    hsot::IEventTransport* transport{};
    {
        std::scoped_lock lock(g_transport_mutex);
        transport = std::exchange(g_transport, nullptr);
    }
    delete transport;
}

} // namespace

// Starts the transport and the publisher in `shape`. Returns 0 when both
// threads run.
extern "C" __declspec(dllexport) int ProbeStart(const int shape) noexcept {
    g_shape = shape;
    std::string detail;
    if (shape == kShapeBeforeFix) {
        g_before_fix_transport = hsot::CreateLocalNamedPipeTransport(512U);
        if (!g_before_fix_transport || !g_before_fix_transport->Start(GetCurrentProcessId(), detail)) {
            return 1;
        }
        try {
            g_before_fix_publish_worker = std::thread(&PublishWorkerLoop);
        } catch (...) {
            return 2;
        }
        return 0;
    }
    g_transport = hsot::CreateLocalNamedPipeTransport(512U).release();
    if (!g_transport || !g_transport->Start(GetCurrentProcessId(), detail)) {
        return 1;
    }
    return g_publish_worker.Start(&PublishWorkerLoop) ? 0 : 2;
}

// The body of ModuleUnload in src/module.cpp. Returns the UnloadOutcome.
extern "C" __declspec(dllexport) int ProbeUnload() noexcept {
    g_stopping.store(true, std::memory_order_release);
    g_publish_wake.notify_all();
    const auto outcome = hsot::aurie::StopForUnload(g_publish_worker, [] {
        StopTransport();
        ReleaseTransport();
    });
    g_last_unload.store(static_cast<int>(outcome));
    return static_cast<int>(outcome);
}

// The UnloadOutcome of the last ProbeUnload, or -1.
extern "C" __declspec(dllexport) int ProbeLastUnload() noexcept {
    return g_last_unload.load();
}

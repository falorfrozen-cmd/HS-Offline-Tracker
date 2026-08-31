#include "hsot/event_transport.h"
#include "hsot/event_protocol.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace hsot {
namespace {

constexpr std::chrono::milliseconds kWriteRetryBackoff[] = {
    std::chrono::milliseconds(25),
    std::chrono::milliseconds(50),
    std::chrono::milliseconds(100),
    std::chrono::milliseconds(200),
    std::chrono::milliseconds(250),
};

class LocalNamedPipeTransport final : public IEventTransport {
public:
    explicit LocalNamedPipeTransport(const std::size_t capacity) noexcept
        : capacity_(capacity == 0U ? 1U : capacity) {}

    ~LocalNamedPipeTransport() override {
        Stop();
    }

    [[nodiscard]] bool Start(const std::uint32_t process_id, std::string& detail) noexcept override {
        std::scoped_lock state_lock(state_mutex_);
        if (started_.load(std::memory_order_acquire)) {
            detail = "transport is already started";
            return true;
        }
        try {
            pipe_name_wide_ = L"\\\\.\\pipe\\HSOfflineTrackerBridge_" + std::to_wstring(process_id);
            pipe_name_utf8_ = "\\\\.\\pipe\\HSOfflineTrackerBridge_" + std::to_string(process_id);
        } catch (...) {
            detail = "failed to allocate pipe name";
            return false;
        }

        pipe_ = CreateNamedPipeW(
            pipe_name_wide_.c_str(),
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1U,
            static_cast<DWORD>(protocol::kMaximumMessageBytes),
            static_cast<DWORD>(protocol::kMaximumMessageBytes),
            0U,
            nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) {
            detail = "CreateNamedPipeW failed: " + std::to_string(GetLastError());
            return false;
        }

        stop_requested_.store(false, std::memory_order_release);
        started_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread([this] { WorkerLoop(); });
        } catch (...) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            started_.store(false, std::memory_order_release);
            detail = "failed to start pipe worker";
            return false;
        }
        detail = "local named pipe worker started";
        return true;
    }

    [[nodiscard]] PublishResult TryPublish(const std::string_view ndjson) noexcept override {
        if (!started_.load(std::memory_order_acquire)) {
            return PublishResult::not_started;
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            return PublishResult::stopping;
        }
        if (ndjson.empty() || ndjson.back() != '\n' || ndjson.find('\0') != std::string_view::npos) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return PublishResult::invalid_message;
        }
        if (ndjson.size() > protocol::kMaximumMessageBytes) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return PublishResult::message_too_large;
        }

        std::unique_lock queue_lock(queue_mutex_, std::try_to_lock);
        if (!queue_lock.owns_lock()) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return PublishResult::busy;
        }
        if (queue_.size() >= capacity_) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return PublishResult::queue_full;
        }
        try {
            queue_.emplace_back(ndjson);
        } catch (...) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return PublishResult::queue_full;
        }
        queued_.fetch_add(1U, std::memory_order_relaxed);
        queue_lock.unlock();
        wake_.notify_one();
        return PublishResult::queued;
    }

    void Stop() noexcept override {
        std::scoped_lock state_lock(state_mutex_);
        if (!started_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        std::scoped_lock queue_lock(queue_mutex_);
        queue_.clear();
    }

    [[nodiscard]] std::string PipeNameUtf8() const override {
        std::scoped_lock state_lock(state_mutex_);
        return pipe_name_utf8_;
    }

    [[nodiscard]] TransportStats Stats() const noexcept override {
        return {
            queued_.load(std::memory_order_relaxed),
            sent_.load(std::memory_order_relaxed),
            dropped_.load(std::memory_order_relaxed),
            reconnects_.load(std::memory_order_relaxed),
            write_errors_.load(std::memory_order_relaxed),
        };
    }

private:
    bool PollConnection() noexcept {
        if (connected_) {
            return true;
        }
        if (ConnectNamedPipe(pipe_, nullptr) != FALSE) {
            connected_ = true;
            reconnects_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected_ = true;
            reconnects_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void DropConnection() noexcept {
        if (connected_) {
            DisconnectNamedPipe(pipe_);
            connected_ = false;
        }
    }

    [[nodiscard]] bool WaitForStop(const std::chrono::milliseconds delay) noexcept {
        std::unique_lock wait_lock(queue_mutex_);
        return wake_.wait_for(wait_lock, delay, [this] {
            return stop_requested_.load(std::memory_order_acquire);
        });
    }

    [[nodiscard]] bool WriteMessage(const std::string& message) noexcept {
        DWORD written{};
        if (WriteFile(pipe_, message.data(), static_cast<DWORD>(message.size()), &written, nullptr) != FALSE &&
            written == message.size()) {
            sent_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        write_errors_.fetch_add(1U, std::memory_order_relaxed);
        DropConnection();
        return false;
    }

    void WorkerLoop() noexcept {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (!PollConnection()) {
                std::unique_lock wait_lock(queue_mutex_);
                wake_.wait_for(wait_lock, std::chrono::milliseconds(10), [this] {
                    return stop_requested_.load(std::memory_order_acquire);
                });
                continue;
            }

            std::string message;
            {
                std::unique_lock queue_lock(queue_mutex_);
                if (queue_.empty()) {
                    wake_.wait_for(queue_lock, std::chrono::milliseconds(10), [this] {
                        return stop_requested_.load(std::memory_order_acquire) || !queue_.empty();
                    });
                    continue;
                }
                // Keep the moved-from front entry in the deque until this message is
                // delivered or exhausts its retry budget. It therefore still counts
                // toward capacity and later messages cannot overtake it.
                message = std::move(queue_.front());
            }

            bool delivered = WriteMessage(message);
            for (const auto retry_delay : kWriteRetryBackoff) {
                if (delivered || stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                if (WaitForStop(retry_delay)) {
                    break;
                }
                if (PollConnection()) {
                    delivered = WriteMessage(message);
                }
            }

            {
                std::scoped_lock queue_lock(queue_mutex_);
                queue_.pop_front();
            }
            if (!delivered && !stop_requested_.load(std::memory_order_acquire)) {
                dropped_.fetch_add(1U, std::memory_order_relaxed);
            }
        }
        DropConnection();
    }

    const std::size_t capacity_;
    mutable std::mutex state_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::thread worker_;
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    std::wstring pipe_name_wide_;
    std::string pipe_name_utf8_;
    bool connected_{};
    std::atomic_bool started_{};
    std::atomic_bool stop_requested_{};
    std::atomic_uint64_t queued_{};
    std::atomic_uint64_t sent_{};
    std::atomic_uint64_t dropped_{};
    std::atomic_uint64_t reconnects_{};
    std::atomic_uint64_t write_errors_{};
};

} // namespace

std::unique_ptr<IEventTransport> CreateLocalNamedPipeTransport(const std::size_t queue_capacity) noexcept {
    try {
        return std::make_unique<LocalNamedPipeTransport>(queue_capacity);
    } catch (...) {
        return nullptr;
    }
}

} // namespace hsot

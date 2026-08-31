#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hsot {

enum class PublishResult {
    queued,
    not_started,
    stopping,
    busy,
    queue_full,
    message_too_large,
    invalid_message,
};

struct TransportStats {
    std::uint64_t queued{};
    std::uint64_t sent{};
    std::uint64_t dropped{};
    std::uint64_t reconnects{};
    std::uint64_t write_errors{};
};

class IEventTransport {
public:
    virtual ~IEventTransport() = default;

    // Creates a local-only server named \\.\pipe\HSOfflineTrackerBridge_<pid>.
    [[nodiscard]] virtual bool Start(std::uint32_t process_id, std::string& detail) noexcept = 0;

    // Producer-side operation. It never performs pipe I/O and does not wait for the queue lock.
    [[nodiscard]] virtual PublishResult TryPublish(std::string_view ndjson) noexcept = 0;

    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual std::string PipeNameUtf8() const = 0;
    [[nodiscard]] virtual TransportStats Stats() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IEventTransport> CreateLocalNamedPipeTransport(
    std::size_t queue_capacity = 512U) noexcept;

} // namespace hsot


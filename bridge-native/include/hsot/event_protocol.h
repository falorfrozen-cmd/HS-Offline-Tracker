#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsot::protocol {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr char kProtocolName[] = "hs-offline-tracker/1";
inline constexpr std::size_t kMaximumMessageBytes = 256U * 1024U;

enum class BridgeState {
    scaffold_ready,
    blocked,
    transport_ready,
    transport_error,
};

struct EventEnvelope {
    std::uint32_t protocol_version{kProtocolVersion};
    std::uint64_t sequence{};
    std::uint64_t emitted_at_unix_ms{};
    std::uint32_t process_id{};
    std::string build_id;
};

struct StatusEvent {
    EventEnvelope envelope;
    BridgeState state{BridgeState::blocked};
    std::string code;
    std::string detail;
};

// Positive, producer-observed changes since the previous emitted batch.  The
// fields are independent so a producer can safely leave an unverified sensor
// at zero without inventing data for it.
struct SessionDeltaEvent {
    EventEnvelope envelope;
    std::int64_t gold{};
    std::int64_t xp{};
    std::int64_t kills{};
    std::string source;
};

// A read-only snapshot of values calculated by the game itself. Producers
// must observe these after the native routine returns and must never alter the
// engine-owned result.
struct VitalsEvent {
    EventEnvelope envelope;
    double magic_find{};
    std::string source;
};

// Monotonic counters used to validate a candidate sensor against persisted
// save progress.  Diagnostics never change tracker totals.
struct SensorDiagnosticEvent {
    EventEnvelope envelope;
    std::uint64_t gold_calls{};
    std::uint64_t gold_accepted{};
    std::uint64_t gold_rejected{};
    std::uint64_t gold_delta_total{};
    std::uint64_t xp_calls{};
    std::uint64_t xp_accepted{};
    std::uint64_t xp_rejected{};
    std::uint64_t xp_delta_total{};
    std::uint64_t kill_candidate_calls{};
    bool kill_emission_enabled{};
    std::string kill_route;
    std::uint64_t drop_calls{};
    std::uint64_t drop_accepted{};
    std::uint64_t drop_rejected{};
    std::uint64_t drop_queue_dropped{};
    std::uint64_t drop_emitted{};
    std::string drop_route;
};

struct ItemProperty {
    std::string key;
    std::string label;
    std::string value;
};

struct GroundDropEvent {
    EventEnvelope envelope;
    std::string event_id;
    std::string source;
    std::optional<std::string> room_name;
    std::optional<double> x;
    std::optional<double> y;

    std::optional<std::string> item_name;
    std::optional<std::int32_t> rarity_id;
    std::optional<std::string> rarity_name;
    std::optional<std::int64_t> tier;
    std::optional<std::int64_t> item_type;
    std::optional<std::int64_t> item_id;
    std::optional<std::int64_t> weapon_type;
    std::optional<std::int64_t> seed;
    std::int64_t amount{1};
    std::optional<double> magic_find;
    std::optional<bool> magic_find_drop;
    std::optional<bool> unscaled;
    std::optional<std::string> item_data_hash;
    std::optional<std::string> fingerprint;
    std::optional<std::string> drop_sound_asset;
    std::vector<ItemProperty> properties;
};

[[nodiscard]] const char* ToString(BridgeState state) noexcept;
[[nodiscard]] std::string SerializeNdjson(const StatusEvent& event);
[[nodiscard]] std::string SerializeNdjson(const SessionDeltaEvent& event);
[[nodiscard]] std::string SerializeNdjson(const VitalsEvent& event);
[[nodiscard]] std::string SerializeNdjson(const SensorDiagnosticEvent& event);
[[nodiscard]] std::string SerializeNdjson(const GroundDropEvent& event);

} // namespace hsot::protocol

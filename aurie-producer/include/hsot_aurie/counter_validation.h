#pragma once

#include <cstdint>
#include <optional>

namespace hsot::aurie {

enum class ObservedNumberKind {
    real,
    int32,
    int64,
    unsupported,
};

struct ObservedNumber {
    ObservedNumberKind kind{ObservedNumberKind::unsupported};
    double real{};
    std::int64_t integer{};
};

// The live counter contract accepts only strictly-positive integral deltas.
// Real values are capped at 2^53-1 so a conversion cannot silently round a
// large GameMaker number before it enters the event stream.
[[nodiscard]] std::optional<std::int64_t> DecodePositiveIntegralDelta(
    const ObservedNumber& value) noexcept;

// Used for identifiers/indexes rather than earned-value deltas. Zero is a
// valid identifier, but undefined, fractional, negative and lossy values are
// rejected.
[[nodiscard]] std::optional<std::int64_t> DecodeNonNegativeIntegralId(
    const ObservedNumber& value) noexcept;

// Checked addition used by the hook-side atomic accumulator.
[[nodiscard]] bool AddPositiveWithoutOverflow(
    std::int64_t current,
    std::int64_t delta,
    std::int64_t& result) noexcept;

// YYC represents an explicitly rejected script result as VALUE_BOOL (13)
// with a zero int32 payload. Undefined and every non-false result mean that
// the original routine did not take its rejection exit.
[[nodiscard]] bool IsExplicitFalseBooleanResult(
    std::uint32_t kind,
    std::int32_t payload) noexcept;

} // namespace hsot::aurie

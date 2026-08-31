#include "hsot_aurie/counter_validation.h"

#include <cmath>
#include <limits>

namespace hsot::aurie {

namespace {

constexpr double kMaximumExactInteger = 9007199254740991.0;

[[nodiscard]] std::optional<std::int64_t> DecodeIntegral(
    const ObservedNumber& value,
    const bool allow_zero) noexcept {
    switch (value.kind) {
    case ObservedNumberKind::int32:
    case ObservedNumberKind::int64:
        if (value.integer > 0 || (allow_zero && value.integer == 0)) {
            return value.integer;
        }
        return std::nullopt;
    case ObservedNumberKind::real:
        if (!std::isfinite(value.real) ||
            value.real < (allow_zero ? 0.0 : 1.0) ||
            value.real > kMaximumExactInteger ||
            std::trunc(value.real) != value.real) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value.real);
    case ObservedNumberKind::unsupported:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::int64_t> DecodePositiveIntegralDelta(const ObservedNumber& value) noexcept {
    return DecodeIntegral(value, false);
}

std::optional<std::int64_t> DecodeNonNegativeIntegralId(const ObservedNumber& value) noexcept {
    return DecodeIntegral(value, true);
}

bool AddPositiveWithoutOverflow(
    const std::int64_t current,
    const std::int64_t delta,
    std::int64_t& result) noexcept {
    if (current < 0 || delta <= 0 || current > (std::numeric_limits<std::int64_t>::max)() - delta) {
        return false;
    }
    result = current + delta;
    return true;
}

bool IsExplicitFalseBooleanResult(
    const std::uint32_t kind,
    const std::int32_t payload) noexcept {
    constexpr std::uint32_t kValueBool = 13U;
    return kind == kValueBool && payload == 0;
}

} // namespace hsot::aurie

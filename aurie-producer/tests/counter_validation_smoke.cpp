#include "hsot_aurie/counter_validation.h"

#include <cstdint>
#include <limits>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using hsot::aurie::DecodePositiveIntegralDelta;
    using hsot::aurie::DecodeNonNegativeIntegralId;
    using hsot::aurie::ObservedNumber;
    using hsot::aurie::ObservedNumberKind;

    REQUIRE(DecodePositiveIntegralDelta({ObservedNumberKind::int32, 0.0, 1250}) == 1250);
    REQUIRE(DecodePositiveIntegralDelta({ObservedNumberKind::int64, 0.0, 42000}) == 42000);
    REQUIRE(DecodePositiveIntegralDelta({ObservedNumberKind::real, 200.0, 0}) == 200);
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::real, 1.5, 0}));
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::real,
        std::numeric_limits<double>::infinity(), 0}));
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::real, 9007199254740992.0, 0}));
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::int64, 0.0, 0}));
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::int64, 0.0, -1}));
    REQUIRE(!DecodePositiveIntegralDelta({ObservedNumberKind::unsupported, 0.0, 1}));

    REQUIRE(DecodeNonNegativeIntegralId({ObservedNumberKind::int32, 0.0, 0}) == 0);
    REQUIRE(DecodeNonNegativeIntegralId({ObservedNumberKind::real, 12.0, 0}) == 12);
    REQUIRE(!DecodeNonNegativeIntegralId({ObservedNumberKind::real, -1.0, 0}));
    REQUIRE(!DecodeNonNegativeIntegralId({ObservedNumberKind::real, 0.5, 0}));
    REQUIRE(!DecodeNonNegativeIntegralId({ObservedNumberKind::unsupported, 0.0, 0}));

    std::int64_t sum{};
    REQUIRE(hsot::aurie::AddPositiveWithoutOverflow(100, 50, sum));
    REQUIRE(sum == 150);
    REQUIRE(!hsot::aurie::AddPositiveWithoutOverflow(
        (std::numeric_limits<std::int64_t>::max)(), 1, sum));
    REQUIRE(!hsot::aurie::AddPositiveWithoutOverflow(10, 0, sum));
    REQUIRE(!hsot::aurie::AddPositiveWithoutOverflow(-1, 1, sum));

    REQUIRE(hsot::aurie::IsExplicitFalseBooleanResult(13U, 0));
    REQUIRE(!hsot::aurie::IsExplicitFalseBooleanResult(13U, 1));
    REQUIRE(!hsot::aurie::IsExplicitFalseBooleanResult(5U, 0));
    REQUIRE(!hsot::aurie::IsExplicitFalseBooleanResult(0U, 0));
    return 0;
}

#include "hsot/build_fingerprint.h"
#include "hsot_aurie/producer_profile.h"

#include <filesystem>
#include <string>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (false)

namespace {

[[nodiscard]] hsot::BuildFingerprint ExpectedFingerprint(
    const hsot::aurie::ProducerProfile& profile) {
    return {
        profile.reference_file_size,
        profile.pe_timestamp,
        profile.machine,
        std::string(profile.known_full_sha256_hex.front()),
        profile.text_raw_size,
        std::string(profile.text_sha256_hex),
    };
}

[[nodiscard]] bool MatchesAnyProfile(const hsot::BuildFingerprint& actual) {
    for (const auto& profile : hsot::aurie::kSupportedProfiles) {
        if (hsot::CodeFingerprintsExactlyMatch(actual, ExpectedFingerprint(profile))) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(const int argument_count, const char* arguments[]) {
    REQUIRE(hsot::aurie::kSupportedProfiles.size() == 2U);
    REQUIRE(hsot::aurie::kCounterRoutesCurrent[2].routine_name ==
        "gml_Script_EnemyAddStatistics");
    REQUIRE(hsot::aurie::kCounterRoutesCurrent[2].expected_argument_count == 1);
    REQUIRE(hsot::aurie::kCounterRoutesCurrent[2].value_argument_index == 0);
    REQUIRE(hsot::aurie::kCounterRoutesCurrent[2].emits_session_delta);
    REQUIRE(hsot::aurie::kCounterRoutesLegacy[0].value_argument_index == 0);
    REQUIRE(hsot::aurie::kCounterRoutesCurrent[0].value_argument_index == 1);
    REQUIRE(hsot::aurie::kGroundDropRouteCurrent.enabled);
    REQUIRE(hsot::aurie::kGroundDropRouteCurrent.routine_name ==
        "gml_Script_LootGroundInit");
    REQUIRE(hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x04C3DB7CU));
    REQUIRE(hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x04C4E150U));
    REQUIRE(hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x04C51259U));
    REQUIRE(hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x04C51AB0U));
    REQUIRE(hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x04D15079U));
    // Explicitly exclude player-discard, API/market and zone-restore callers.
    REQUIRE(!hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x003549EAU));
    REQUIRE(!hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x0703FD3BU));
    REQUIRE(!hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x0704BFB5U));
    REQUIRE(!hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteCurrent, 0x0B459474U));
    REQUIRE(!hsot::aurie::IsGroundItemCreateCallerRva(
        hsot::aurie::kGroundDropRouteUnavailable, 0x04C51259U));
    REQUIRE(hsot::aurie::IsTrackedRareRarity(4));
    REQUIRE(hsot::aurie::IsTrackedRareRarity(6));
    REQUIRE(hsot::aurie::IsTrackedRareRarity(7));
    REQUIRE(hsot::aurie::IsTrackedRareRarity(9));
    REQUIRE(hsot::aurie::IsTrackedRareRarity(10));
    REQUIRE(!hsot::aurie::IsTrackedRareRarity(0));
    REQUIRE(!hsot::aurie::IsTrackedRareRarity(8));
    REQUIRE(!hsot::aurie::IsTrackedRareRarity(11));
    const auto& legacy = hsot::aurie::kSupportedProfiles[0];
    REQUIRE(legacy.pe_timestamp == 0x6A8DDE33U);
    REQUIRE(legacy.machine == 0x8664U);
    REQUIRE(legacy.text_raw_size == 232739840U);
    REQUIRE(legacy.text_sha256_hex ==
        "1dde65e44f2d8f90158b290dc60b71f80a4717393f2488a254bb7871e218d917");
    const auto& current = hsot::aurie::kSupportedProfiles[1];
    REQUIRE(current.reference_file_size == 281599488ULL);
    REQUIRE(current.pe_timestamp == 0x6A91A8B3U);
    REQUIRE(current.machine == 0x8664U);
    REQUIRE(current.text_raw_size == 212810240U);
    REQUIRE(current.text_sha256_hex ==
        "c4dc91d9dfa4982ac8b9044f2f88fd6ed64a7b84cd641bc20c4517c6129d18fb");

    REQUIRE(!legacy.drop_route.enabled);
    REQUIRE(current.drop_route.enabled);
    REQUIRE(current.drop_route.routine_name ==
        "gml_Script_LootGroundInit");
    REQUIRE(!legacy.magic_find_route.enabled);
    REQUIRE(!current.magic_find_route.enabled);
    REQUIRE(current.magic_find_route.routine_name.empty());
    REQUIRE(current.magic_find_route.hook_id.empty());

    for (const auto& profile : hsot::aurie::kSupportedProfiles) {
        const auto expected = ExpectedFingerprint(profile);
        auto synthetic = expected;
        REQUIRE(MatchesAnyProfile(synthetic));

        // Full-file differences (including Aurie's own section) are diagnostics.
        synthetic.file_size += 4096U;
        synthetic.sha256_hex = "different-full-file-hash";
        REQUIRE(hsot::CodeFingerprintsExactlyMatch(synthetic, expected));

        synthetic = expected;
        ++synthetic.pe_timestamp;
        REQUIRE(!hsot::CodeFingerprintsExactlyMatch(synthetic, expected));
        synthetic = expected;
        synthetic.machine = 0x014cU;
        REQUIRE(!hsot::CodeFingerprintsExactlyMatch(synthetic, expected));
        synthetic = expected;
        --synthetic.text_raw_size;
        REQUIRE(!hsot::CodeFingerprintsExactlyMatch(synthetic, expected));
        synthetic = expected;
        synthetic.text_sha256_hex[0] = synthetic.text_sha256_hex[0] == '0' ? '1' : '0';
        REQUIRE(!hsot::CodeFingerprintsExactlyMatch(synthetic, expected));
    }

    for (int index = 1; index < argument_count; ++index) {
        const auto actual = hsot::FingerprintExecutable(std::filesystem::path(arguments[index]));
        REQUIRE(actual.ok);
        REQUIRE(MatchesAnyProfile(actual.fingerprint));
    }
    return 0;
}

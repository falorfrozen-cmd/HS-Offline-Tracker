#include "hsot/build_fingerprint.h"

#include <filesystem>
#include <string>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (false)

int main(const int argument_count, const char* arguments[]) {
    const auto profiles = hsot::EmbeddedSupportedBuilds();
    REQUIRE(profiles.size() == 1U);
    const auto& profile = profiles.front();
    REQUIRE(profile.fingerprint.file_size == 303584768ULL);
    REQUIRE(profile.fingerprint.sha256_hex ==
        "5f8085456a27109681403d8c57533e6999fbd0664752ff5f2856985b5fbbde71");
    REQUIRE(profile.fingerprint.pe_timestamp == 0x6A8C4540U);
    REQUIRE(profile.fingerprint.machine == 0x8664U);
    REQUIRE(profile.fingerprint.text_raw_size == 232640000U);
    REQUIRE(profile.fingerprint.text_sha256_hex ==
        "e6c8c585dac9d35615210ed575c2ce3ba7eda9f8b72b27dabd5ce7f527d7736e");
    REQUIRE(profile.expected_announcement_argument_count == 3);
    REQUIRE(profile.expected_rarity_key == 27);
    REQUIRE(profile.rarity_argument_index == 0U);
    REQUIRE(profile.item_type_argument_index == 1U);
    REQUIRE(profile.item_id_argument_index == 2U);

    hsot::FingerprintResult synthetic{};
    synthetic.ok = true;
    synthetic.fingerprint = profile.fingerprint;
    REQUIRE(hsot::EvaluateBuild(synthetic, profiles).code == hsot::BuildGateCode::allowed);
    REQUIRE(hsot::CodeFingerprintsExactlyMatch(
        synthetic.fingerprint, profile.fingerprint));
    synthetic.fingerprint.sha256_hex[0] = synthetic.fingerprint.sha256_hex[0] == '0' ? '1' : '0';
    REQUIRE(hsot::EvaluateBuild(synthetic, profiles).code == hsot::BuildGateCode::unsupported_build);
    REQUIRE(hsot::CodeFingerprintsExactlyMatch(
        synthetic.fingerprint, profile.fingerprint));
    synthetic.fingerprint.text_sha256_hex[0] =
        synthetic.fingerprint.text_sha256_hex[0] == '0' ? '1' : '0';
    REQUIRE(!hsot::CodeFingerprintsExactlyMatch(
        synthetic.fingerprint, profile.fingerprint));

    if (argument_count == 2) {
        const auto actual = hsot::FingerprintExecutable(std::filesystem::path(arguments[1]));
        REQUIRE(actual.ok);
        REQUIRE(hsot::EvaluateBuild(actual, profiles).code == hsot::BuildGateCode::allowed);
    }
    return 0;
}

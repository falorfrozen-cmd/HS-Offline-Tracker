#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace hsot {

struct BuildFingerprint {
    std::uint64_t file_size{};
    std::uint32_t pe_timestamp{};
    std::uint16_t machine{};
    std::string sha256_hex;
    std::uint32_t text_raw_size{};
    std::string text_sha256_hex;
};

struct FingerprintResult {
    bool ok{};
    BuildFingerprint fingerprint;
    std::string detail;
};

struct SupportedBuild {
    std::string build_id;
    BuildFingerprint fingerprint;
    std::vector<std::string> required_routine_names;

    // Profile-bound semantic limits. These are byte counts, never route RVAs.
    std::string ground_create_name;
    std::string announcement_caller_name;
    std::string rare_announcement_name;
    std::size_t ground_create_bytes{};
    std::size_t announcement_caller_bytes{};
    std::size_t rare_announcement_bytes{};
    std::int32_t expected_announcement_argument_count{};
    std::int64_t expected_rarity_key{};
    std::size_t rarity_argument_index{};
    std::size_t item_type_argument_index{};
    std::size_t item_id_argument_index{};
};

enum class BuildGateCode {
    allowed,
    fingerprint_error,
    unsupported_build,
};

struct BuildGateResult {
    BuildGateCode code{BuildGateCode::unsupported_build};
    const SupportedBuild* profile{};
    std::string detail;
};

[[nodiscard]] FingerprintResult FingerprintExecutable(
    const std::filesystem::path& executable_path) noexcept;

[[nodiscard]] bool FingerprintsExactlyMatch(
    const BuildFingerprint& actual,
    const BuildFingerprint& expected) noexcept;

// Aurie may rewrite/add its own non-code section while preserving the exact
// game code. This matcher remains fail-closed on PE identity and the complete
// on-disk .text raw section, but deliberately treats the full-file hash/size
// as diagnostics rather than gate inputs.
[[nodiscard]] bool CodeFingerprintsExactlyMatch(
    const BuildFingerprint& actual,
    const BuildFingerprint& expected) noexcept;

[[nodiscard]] BuildGateResult EvaluateBuild(
    const FingerprintResult& actual,
    std::span<const SupportedBuild> supported_builds) noexcept;

// Contains only exact, reviewed test builds. A hash mismatch is a hard failure.
[[nodiscard]] std::span<const SupportedBuild> EmbeddedSupportedBuilds() noexcept;

} // namespace hsot

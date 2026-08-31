#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace hsot::aurie {

enum class SensorKind {
    gold,
    xp,
    kill_candidate,
};

struct NamedRouteProfile {
    std::string_view routine_name;
    std::string_view hook_id;
    std::int32_t expected_argument_count{};
    std::int32_t value_argument_index{};
    SensorKind sensor{};
    bool emits_session_delta{};
};

struct DropRouteProfile {
    bool enabled{};
    std::string_view routine_name;
    std::string_view hook_id;
    std::array<std::uint32_t, 5U> ground_caller_return_rvas{};
};

struct VitalsRouteProfile {
    bool enabled{};
    std::string_view routine_name;
    std::string_view hook_id;
};

struct ProducerProfile {
    std::string_view build_id;
    std::uint64_t reference_file_size{};
    std::uint32_t pe_timestamp{};
    std::uint16_t machine{};
    std::uint32_t text_raw_size{};
    std::string_view text_sha256_hex;
    std::array<std::string_view, 2U> known_full_sha256_hex;
    std::array<NamedRouteProfile, 3U> routes;
    DropRouteProfile drop_route;
    VitalsRouteProfile magic_find_route;
};

// The tracked values are the resolved S10 rarity IDs stored in itemInfoStruct[27].
[[nodiscard]] inline constexpr bool IsTrackedRareRarity(
    const std::int64_t rarity) noexcept {
    return rarity == 4 || rarity == 6 || rarity == 7 || rarity == 9 || rarity == 10;
}

// LootGroundInit is the final common placement step. It has nine direct callers
// in this exact build. These five are the reviewed local drop-generation paths:
// LootExplosion, LootGroundDrop, LootGroundCreate, LootGroundCreateFromItem and
// CreateItemDrop. The excluded call sites are CA_playerItemDrop, two API/market
// reconstruction paths and Zone_State_Buffer restore. This keeps player-discarded
// and pre-existing floor items out of the session counter.
[[nodiscard]] inline constexpr bool IsGroundItemCreateCallerRva(
    const DropRouteProfile& profile,
    const std::uint32_t caller_return_rva) noexcept {
    if (!profile.enabled || caller_return_rva == 0U) {
        return false;
    }
    for (const auto reviewed_rva : profile.ground_caller_return_rvas) {
        if (reviewed_rva != 0U && caller_return_rva == reviewed_rva) {
            return true;
        }
    }
    return false;
}

inline constexpr DropRouteProfile kGroundDropRouteCurrent{
    true,
    "gml_Script_LootGroundInit",
    "hsot_ground_item_init",
    {{
        0x04C3DB7CU,
        0x04C4E150U,
        0x04C51259U,
        0x04C51AB0U,
        0x04D15079U,
    }},
};

// The earlier counter profile remains useful for Gold/XP/Kills, but its drop
// caller/ABI was not statically re-reviewed. Keep drops fail-closed there.
inline constexpr DropRouteProfile kGroundDropRouteUnavailable{
    false,
    "",
    "",
    {{0U, 0U, 0U, 0U, 0U}},
};

// StatMagicFind VALUE_ARRAY element 0 was observed as 351 while the same
// character panel displayed 624%. It is a genuine internal value, but not the
// verified final panel total. Keep this sensor fail-closed on every profile
// until the later player/panel calculation has been identified.
inline constexpr VitalsRouteProfile kMagicFindRouteUnavailable{
    false,
    "",
    "",
};

inline constexpr std::array<NamedRouteProfile, 3U> kCounterRoutesLegacy{{
    {"gml_Script_GoldLogAdd", "hsot_gold_log_add", 2, 0, SensorKind::gold, true},
    {"gml_Script_ExperienceUpdate", "hsot_experience_update", 2, 0, SensorKind::xp, true},
    {"gml_Script_EnemyAddStatistics", "hsot_enemy_add_statistics", 1, 0,
        SensorKind::kill_candidate, true},
}};

// The 2026-08-29 S10 build kept the GoldLogAdd name and arity but moved the
// earned amount from argument 0 to argument 1. Keep this ABI difference tied
// to the exact code fingerprint; silently falling back between arguments can
// turn a numeric currency/action ID into fake session gold.
inline constexpr std::array<NamedRouteProfile, 3U> kCounterRoutesCurrent{{
    {"gml_Script_GoldLogAdd", "hsot_gold_log_add", 2, 1, SensorKind::gold, true},
    {"gml_Script_ExperienceUpdate", "hsot_experience_update", 2, 0, SensorKind::xp, true},
    {"gml_Script_EnemyAddStatistics", "hsot_enemy_add_statistics", 1, 0,
        SensorKind::kill_candidate, true},
}};

// Exact code profiles generated from reviewed S10 executables. Aurie may alter
// its own non-code section, so full-file hashes remain diagnostics. A PE
// identity or complete on-disk .text mismatch still blocks every hook.
inline constexpr std::array<ProducerProfile, 2U> kSupportedProfiles{{
    {
        "S10-code-1dde65e4",
        303708672ULL,
        0x6A8DDE33U,
        0x8664U,
        232739840U,
        "1dde65e44f2d8f90158b290dc60b71f80a4717393f2488a254bb7871e218d917",
        {{
            "8046293cc8df1c735c680860d3f26d33c6993afb2a2469bac7f1d1d2ee882f6b",
            "06b103314f5f472d0ebc17b64caa7d73f9d9ba683ebeb71379ad97f95d813df2",
        }},
        kCounterRoutesLegacy,
        kGroundDropRouteUnavailable,
        kMagicFindRouteUnavailable,
    },
    {
        "S10-code-c4dc91d9",
        281599488ULL,
        0x6A91A8B3U,
        0x8664U,
        212810240U,
        "c4dc91d9dfa4982ac8b9044f2f88fd6ed64a7b84cd641bc20c4517c6129d18fb",
        {{
            "438bf4848688c5be52ac15f26f02b46da620d90587c28e766a9cea190f3a7de4",
            "",
        }},
        kCounterRoutesCurrent,
        kGroundDropRouteCurrent,
        kMagicFindRouteUnavailable,
    },
}};

} // namespace hsot::aurie

#pragma once

#include "hsot/build_fingerprint.h"
#include "hsot/named_routine_resolver.h"

#include <cstdint>
#include <string>

namespace hsot {

struct ValidatedDropRoute {
    std::uintptr_t ground_create{};
    std::uintptr_t announcement_caller{};
    std::uintptr_t rare_announcement{};
    std::uintptr_t direct_call_site{};
};

struct DropRouteValidation {
    bool ok{};
    ValidatedDropRoute route;
    std::string detail;
};

// Read-only semantic validation for the exact build profile. It requires:
//  - all three names to resolve uniquely;
//  - one ground-create RIP-relative reference to the named caller;
//  - one direct E8 call from that caller to the announcement function;
//  - a profile-matching argument-count setup and rarity-key RValue;
//  - every scanned span and resolved pointer to remain inside the module image.
[[nodiscard]] DropRouteValidation ValidateDropRoute(
    const void* module_base,
    const INamedRoutineResolver& resolver,
    const SupportedBuild& profile) noexcept;

} // namespace hsot


#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hsot {

enum class ResolveCode {
    found,
    invalid_module,
    missing,
    ambiguous,
};

struct ResolveResult {
    ResolveCode code{ResolveCode::missing};
    std::uintptr_t address{};
    std::size_t unique_matches{};
    std::string detail;
};

class INamedRoutineResolver {
public:
    virtual ~INamedRoutineResolver() = default;
    [[nodiscard]] virtual ResolveResult ResolveExact(std::string_view name) const noexcept = 0;
};

struct RequiredNameFailure {
    std::string name;
    ResolveCode code{ResolveCode::missing};
    std::string detail;
};

struct RequiredNameValidation {
    bool ok{};
    std::vector<RequiredNameFailure> failures;
};

// module_base must be the base of a PE image already loaded in this process.
// Resolution is read-only and accepts only name pointers inside readable image sections
// and code pointers inside executable image sections.
[[nodiscard]] std::unique_ptr<INamedRoutineResolver> CreateRegistrationTableResolver(
    const void* module_base) noexcept;

[[nodiscard]] RequiredNameValidation ValidateRequiredNames(
    const INamedRoutineResolver& resolver,
    std::span<const std::string> required_names) noexcept;

} // namespace hsot


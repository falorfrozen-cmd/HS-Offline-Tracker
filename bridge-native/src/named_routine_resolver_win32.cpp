#include "hsot/named_routine_resolver.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace hsot {
namespace {

struct ImageRange {
    std::uintptr_t begin{};
    std::uintptr_t end{};
};

bool Contains(const ImageRange& range, const std::uintptr_t address, const std::size_t bytes = 1U) noexcept {
    if (address < range.begin || address > range.end) {
        return false;
    }
    return bytes <= range.end - address;
}

bool InAnyRange(const std::span<const ImageRange> ranges, const std::uintptr_t address,
    const std::size_t bytes = 1U) noexcept {
    return std::ranges::any_of(ranges, [address, bytes](const ImageRange& range) {
        return Contains(range, address, bytes);
    });
}

class RegistrationTableResolver final : public INamedRoutineResolver {
public:
    explicit RegistrationTableResolver(const void* module_base) noexcept {
        Parse(module_base);
    }

    [[nodiscard]] ResolveResult ResolveExact(const std::string_view name) const noexcept override {
        if (!valid_) {
            return {ResolveCode::invalid_module, 0U, 0U, error_};
        }
        if (name.empty() || name.size() > 1024U) {
            return {ResolveCode::missing, 0U, 0U, "routine name is empty or too long"};
        }

        std::vector<std::uintptr_t> matches;
        try {
            for (const auto& table : data_ranges_) {
                if (table.end - table.begin < 3U * sizeof(std::uintptr_t)) {
                    continue;
                }
                const auto final_row = table.end - 3U * sizeof(std::uintptr_t);
                for (auto row = table.begin; row <= final_row; row += sizeof(std::uintptr_t)) {
                    std::uintptr_t name_pointer{};
                    std::uintptr_t code_pointer{};
                    std::memcpy(&name_pointer,
                        reinterpret_cast<const void*>(row + sizeof(std::uintptr_t)), sizeof(name_pointer));
                    std::memcpy(&code_pointer,
                        reinterpret_cast<const void*>(row + 2U * sizeof(std::uintptr_t)), sizeof(code_pointer));

                    if (!InAnyRange(readable_ranges_, name_pointer, name.size() + 1U) ||
                        !InAnyRange(executable_ranges_, code_pointer)) {
                        continue;
                    }
                    const auto* candidate = reinterpret_cast<const char*>(name_pointer);
                    if (std::memcmp(candidate, name.data(), name.size()) == 0 && candidate[name.size()] == '\0') {
                        matches.push_back(code_pointer);
                    }
                }
            }
        } catch (...) {
            return {ResolveCode::invalid_module, 0U, 0U, "exception while scanning registration data"};
        }

        std::ranges::sort(matches);
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        if (matches.empty()) {
            return {ResolveCode::missing, 0U, 0U, "exact routine name was not found"};
        }
        if (matches.size() != 1U) {
            return {ResolveCode::ambiguous, 0U, matches.size(), "exact routine name resolved to multiple code addresses"};
        }
        return {ResolveCode::found, matches.front(), 1U, "unique exact-name match"};
    }

private:
    void Parse(const void* module_base) noexcept {
        if (module_base == nullptr) {
            error_ = "null module base";
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(module_base);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            error_ = "invalid DOS header";
            return;
        }
        const auto nt_address = base + static_cast<std::uintptr_t>(dos->e_lfanew);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(nt_address);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
            error_ = "host is not a valid AMD64 PE image";
            return;
        }

        const auto image_size = static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
        if (image_size < nt->OptionalHeader.SizeOfHeaders || image_size > std::numeric_limits<std::uint32_t>::max()) {
            error_ = "invalid PE image size";
            return;
        }
        const ImageRange image{base, base + image_size};
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            const auto virtual_size = std::max(section[index].Misc.VirtualSize, section[index].SizeOfRawData);
            if (virtual_size == 0U) {
                continue;
            }
            const auto begin = base + static_cast<std::uintptr_t>(section[index].VirtualAddress);
            const auto bytes = static_cast<std::size_t>(std::min<std::uintptr_t>(
                virtual_size, image.end > begin ? image.end - begin : 0U));
            if (bytes == 0U || !Contains(image, begin, bytes)) {
                error_ = "section falls outside loaded image";
                readable_ranges_.clear();
                executable_ranges_.clear();
                data_ranges_.clear();
                return;
            }
            const ImageRange range{begin, begin + bytes};
            const auto flags = section[index].Characteristics;
            if ((flags & IMAGE_SCN_MEM_READ) != 0U) {
                readable_ranges_.push_back(range);
            }
            if ((flags & IMAGE_SCN_MEM_EXECUTE) != 0U) {
                executable_ranges_.push_back(range);
            }
            if ((flags & IMAGE_SCN_MEM_READ) != 0U && (flags & IMAGE_SCN_MEM_EXECUTE) == 0U) {
                data_ranges_.push_back(range);
            }
        }

        if (readable_ranges_.empty() || executable_ranges_.empty() || data_ranges_.empty()) {
            error_ = "required PE section classes are missing";
            return;
        }
        valid_ = true;
    }

    bool valid_{};
    std::string error_{"invalid module"};
    std::vector<ImageRange> readable_ranges_;
    std::vector<ImageRange> executable_ranges_;
    std::vector<ImageRange> data_ranges_;
};

} // namespace

std::unique_ptr<INamedRoutineResolver> CreateRegistrationTableResolver(const void* module_base) noexcept {
    try {
        return std::make_unique<RegistrationTableResolver>(module_base);
    } catch (...) {
        return nullptr;
    }
}

RequiredNameValidation ValidateRequiredNames(
    const INamedRoutineResolver& resolver,
    const std::span<const std::string> required_names) noexcept {
    RequiredNameValidation validation{};
    try {
        for (const auto& name : required_names) {
            const auto result = resolver.ResolveExact(name);
            if (result.code != ResolveCode::found) {
                validation.failures.push_back({name, result.code, result.detail});
            }
        }
        validation.ok = validation.failures.empty();
    } catch (...) {
        validation.ok = false;
        validation.failures.push_back({"<validation>", ResolveCode::invalid_module, "unexpected validation exception"});
    }
    return validation;
}

} // namespace hsot


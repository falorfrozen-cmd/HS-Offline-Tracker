#include "hsot/route_validation.h"

#include "hsot/gamemaker_readonly.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace hsot {
namespace {

struct ImageSection {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    bool readable{};
    bool executable{};
};

struct ImageLayout {
    std::uintptr_t base{};
    std::uintptr_t end{};
    std::vector<ImageSection> sections;
};

bool IsReadableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) return false;
    const auto basic = protection & 0xFFU;
    return basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
        basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadableMemory(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0U) return false;
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (cursor > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0U ||
            memory.State != MEM_COMMIT || !IsReadableProtection(memory.Protect)) {
            return false;
        }
        const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (region_end <= cursor) return false;
        cursor = (std::min)(region_end, end);
    }
    return true;
}

bool ParseImage(const void* module_base, ImageLayout& image, std::string& detail) noexcept {
    if (!IsReadableMemory(module_base, sizeof(IMAGE_DOS_HEADER))) {
        detail = "module DOS header is unreadable";
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module_base);
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, module_base, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) {
        detail = "module DOS header is invalid";
        return false;
    }
    const auto* nt_address = reinterpret_cast<const void*>(base + static_cast<std::uintptr_t>(dos.e_lfanew));
    if (!IsReadableMemory(nt_address, sizeof(IMAGE_NT_HEADERS64))) {
        detail = "module NT header is unreadable";
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, nt_address, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.NumberOfSections == 0U ||
        nt.FileHeader.NumberOfSections > 96U || nt.OptionalHeader.SizeOfImage == 0U) {
        detail = "module is not a valid AMD64 PE image";
        return false;
    }
    if (base > std::numeric_limits<std::uintptr_t>::max() - nt.OptionalHeader.SizeOfImage) {
        detail = "module image range overflows";
        return false;
    }
    image.base = base;
    image.end = base + nt.OptionalHeader.SizeOfImage;

    const auto section_table = base + static_cast<std::uintptr_t>(dos.e_lfanew) +
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    const auto section_bytes = static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (!IsReadableMemory(reinterpret_cast<const void*>(section_table), section_bytes)) {
        detail = "module section table is unreadable";
        return false;
    }
    for (std::uint16_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        std::memcpy(&section,
            reinterpret_cast<const void*>(section_table + index * sizeof(IMAGE_SECTION_HEADER)), sizeof(section));
        const auto size = static_cast<std::uintptr_t>((std::max)(section.Misc.VirtualSize, section.SizeOfRawData));
        if (size == 0U) continue;
        const auto begin = base + section.VirtualAddress;
        if (begin < base || begin >= image.end || size > image.end - begin) {
            detail = "module section lies outside SizeOfImage";
            return false;
        }
        image.sections.push_back({
            begin,
            begin + size,
            (section.Characteristics & IMAGE_SCN_MEM_READ) != 0U,
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0U,
        });
    }
    return !image.sections.empty();
}

bool SpanInSection(const ImageLayout& image, const std::uintptr_t address, const std::size_t bytes,
    const bool require_read, const bool require_execute) noexcept {
    if (bytes == 0U || address > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;
    const auto end = address + bytes;
    return std::ranges::any_of(image.sections, [=](const ImageSection& section) {
        return (!require_read || section.readable) && (!require_execute || section.executable) &&
            address >= section.begin && end <= section.end;
    });
}

std::int32_t ReadRelative32(const std::uint8_t* source) noexcept {
    std::int32_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

std::size_t CountDirectCalls(
    const std::span<const std::uint8_t> code,
    const std::uintptr_t code_address,
    const std::uintptr_t target,
    std::uintptr_t* first_call = nullptr) noexcept {
    std::size_t matches{};
    for (std::size_t offset = 0; offset + 5U <= code.size(); ++offset) {
        if (code[offset] != 0xE8U) continue;
        const auto displacement = ReadRelative32(code.data() + offset + 1U);
        const auto destination = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(code_address + offset + 5U) + displacement);
        if (destination == target) {
            if (matches == 0U && first_call != nullptr) *first_call = code_address + offset;
            ++matches;
        }
    }
    return matches;
}

std::size_t CountRipRelativeLeaTargets(
    const std::span<const std::uint8_t> code,
    const std::uintptr_t code_address,
    const std::uintptr_t target) noexcept {
    std::size_t matches{};
    for (std::size_t offset = 0; offset + 7U <= code.size(); ++offset) {
        const auto rex = code[offset];
        const auto modrm = code[offset + 2U];
        if ((rex & 0xF8U) != 0x48U || code[offset + 1U] != 0x8DU || (modrm & 0xC7U) != 0x05U) {
            continue;
        }
        const auto displacement = ReadRelative32(code.data() + offset + 3U);
        const auto destination = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(code_address + offset + 7U) + displacement);
        if (destination == target) ++matches;
    }
    return matches;
}

bool ContainsPattern(const std::span<const std::uint8_t> bytes, const std::span<const std::uint8_t> pattern) noexcept {
    return !pattern.empty() && std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

bool HasProfileRarityKey(
    const ImageLayout& image,
    const std::span<const std::uint8_t> caller,
    const std::uintptr_t caller_address,
    const std::size_t call_offset,
    const std::int64_t expected_key) noexcept {
    const auto begin = call_offset > 0x300U ? call_offset - 0x300U : 0U;
    for (std::size_t offset = begin; offset + 7U <= call_offset; ++offset) {
        // lea rdx, [rip+disp32]
        if (caller[offset] != 0x48U || caller[offset + 1U] != 0x8DU || caller[offset + 2U] != 0x15U) {
            continue;
        }
        const auto displacement = ReadRelative32(caller.data() + offset + 3U);
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(caller_address + offset + 7U) + displacement);
        if (!SpanInSection(image, target, sizeof(gm::Value), true, false) ||
            !IsReadableMemory(reinterpret_cast<const void*>(target), sizeof(gm::Value))) {
            continue;
        }
        gm::Value constant{};
        std::memcpy(&constant, reinterpret_cast<const void*>(target), sizeof(constant));
        if ((constant.kind & 0x1FU) == static_cast<std::uint32_t>(gm::ValueKind::int64) &&
            constant.flags == 0U && constant.payload.int64_value == expected_key) {
            return true;
        }
    }
    return false;
}

} // namespace

DropRouteValidation ValidateDropRoute(
    const void* module_base,
    const INamedRoutineResolver& resolver,
    const SupportedBuild& profile) noexcept {
    DropRouteValidation validation{};
    try {
        ImageLayout image{};
        if (!ParseImage(module_base, image, validation.detail)) return validation;

        const auto ground = resolver.ResolveExact(profile.ground_create_name);
        const auto caller = resolver.ResolveExact(profile.announcement_caller_name);
        const auto announcement = resolver.ResolveExact(profile.rare_announcement_name);
        if (ground.code != ResolveCode::found || caller.code != ResolveCode::found ||
            announcement.code != ResolveCode::found) {
            validation.detail = "one or more drop-route names did not resolve uniquely";
            return validation;
        }
        if (!SpanInSection(image, ground.address, profile.ground_create_bytes, true, true) ||
            !SpanInSection(image, caller.address, profile.announcement_caller_bytes, true, true) ||
            !SpanInSection(image, announcement.address, profile.rare_announcement_bytes, true, true)) {
            validation.detail = "a named drop-route span is not wholly inside one executable section";
            return validation;
        }

        const auto ground_bytes = std::span(
            reinterpret_cast<const std::uint8_t*>(ground.address), profile.ground_create_bytes);
        const auto caller_bytes = std::span(
            reinterpret_cast<const std::uint8_t*>(caller.address), profile.announcement_caller_bytes);
        const auto announcement_bytes = std::span(
            reinterpret_cast<const std::uint8_t*>(announcement.address), profile.rare_announcement_bytes);

        if (CountRipRelativeLeaTargets(ground_bytes, ground.address, caller.address) != 1U) {
            validation.detail = "ground-create does not contain exactly one RIP-relative reference to the named caller";
            return validation;
        }
        std::uintptr_t call_site{};
        if (CountDirectCalls(caller_bytes, caller.address, announcement.address, &call_site) != 1U) {
            validation.detail = "named caller does not contain exactly one direct call to rare announcement";
            return validation;
        }

        std::size_t module_callers{};
        for (const auto& section : image.sections) {
            if (!section.readable || !section.executable) continue;
            const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(section.begin), section.end - section.begin);
            module_callers += CountDirectCalls(bytes, section.begin, announcement.address);
            if (module_callers > 1U) break;
        }
        if (module_callers != 1U) {
            validation.detail = "rare announcement direct caller is not unique across executable sections";
            return validation;
        }

        const auto call_offset = static_cast<std::size_t>(call_site - caller.address);
        const auto contract_begin = call_offset > 64U ? call_offset - 64U : 0U;
        const auto contract = caller_bytes.subspan(contract_begin, call_offset - contract_begin);
        const std::array<std::uint8_t, 6U> argument_count_pattern{
            0x41U, 0xB9U,
            static_cast<std::uint8_t>(profile.expected_announcement_argument_count), 0U, 0U, 0U,
        };
        constexpr std::array<std::uint8_t, 5U> argument_array_pattern{0x48U, 0x89U, 0x44U, 0x24U, 0x20U};
        constexpr std::array<std::uint8_t, 5U> result_slot_pattern{0x4CU, 0x8DU, 0x44U, 0x24U, 0x30U};
        if (!ContainsPattern(contract, argument_count_pattern) ||
            !ContainsPattern(contract, argument_array_pattern) ||
            !ContainsPattern(contract, result_slot_pattern)) {
            validation.detail = "rare-announcement call-site ABI contract did not match the reviewed profile";
            return validation;
        }
        if (!HasProfileRarityKey(image, caller_bytes, caller.address, call_offset, profile.expected_rarity_key)) {
            validation.detail = "reviewed itemInfo rarity-key RValue was not found before the direct call";
            return validation;
        }

        constexpr std::array<std::uint8_t, 3U> save_argument_count{0x45U, 0x8BU, 0xE9U};
        constexpr std::array<std::uint8_t, 3U> save_result_pointer{0x4DU, 0x8BU, 0xF8U};
        constexpr std::array<std::uint8_t, 4U> load_argument_array{0x4CU, 0x8BU, 0x65U, 0x50U};
        constexpr std::array<std::uint8_t, 8U> boolean_result_kind{
            0x41U, 0xC7U, 0x47U, 0x0CU, 0x0DU, 0x00U, 0x00U, 0x00U,
        };
        constexpr std::array<std::uint8_t, 10U> boolean_true_payload{
            0x48U, 0xB8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xF0U, 0x3FU,
        };
        const auto announcement_prologue = announcement_bytes.first(
            (std::min)(announcement_bytes.size(), std::size_t{128U}));
        if (!ContainsPattern(announcement_prologue, save_argument_count) ||
            !ContainsPattern(announcement_prologue, save_result_pointer) ||
            !ContainsPattern(announcement_prologue, load_argument_array) ||
            !ContainsPattern(announcement_bytes, boolean_result_kind) ||
            !ContainsPattern(announcement_bytes, boolean_true_payload)) {
            validation.detail = "rare-announcement function ABI prologue did not match the reviewed profile";
            return validation;
        }

        validation.ok = true;
        validation.route = {ground.address, caller.address, announcement.address, call_site};
        validation.detail = "exact-name route, unique direct caller, and read-only item argument contract validated";
        return validation;
    } catch (...) {
        validation.ok = false;
        validation.detail = "unexpected route-validation exception";
        return validation;
    }
}

} // namespace hsot

#include "hsot/build_fingerprint.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace hsot {
namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct AlgorithmCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle), 0U);
        }
    }
};

struct HashCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
        }
    }
};

using UniqueAlgorithm = std::unique_ptr<void, AlgorithmCloser>;
using UniqueHash = std::unique_ptr<void, HashCloser>;

bool ReadExactly(HANDLE file, void* destination, const DWORD bytes, const std::uint64_t offset) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    DWORD read{};
    return ReadFile(file, destination, bytes, &read, nullptr) != FALSE && read == bytes;
}

std::string HexLower(const std::span<const std::byte> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        result[index * 2U] = kHex[(value >> 4U) & 0x0FU];
        result[index * 2U + 1U] = kHex[value & 0x0FU];
    }
    return result;
}

bool HashRange(
    HANDLE file,
    const std::uint64_t offset,
    const std::uint64_t length,
    std::string& digest_hex,
    std::string& detail) {
    if (length == 0U) {
        detail = "refusing to hash an empty range";
        return false;
    }

    BCRYPT_ALG_HANDLE raw_algorithm{};
    if (BCryptOpenAlgorithmProvider(&raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U) < 0) {
        detail = "BCryptOpenAlgorithmProvider failed";
        return false;
    }
    UniqueAlgorithm algorithm(raw_algorithm);

    DWORD object_size{};
    DWORD hash_size{};
    DWORD copied{};
    if (BCryptGetProperty(raw_algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0U) < 0 ||
        BCryptGetProperty(raw_algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &copied, 0U) < 0 ||
        hash_size == 0U) {
        detail = "BCryptGetProperty failed";
        return false;
    }

    std::vector<UCHAR> hash_object(object_size);
    BCRYPT_HASH_HANDLE raw_hash{};
    if (BCryptCreateHash(raw_algorithm, &raw_hash, hash_object.data(), object_size, nullptr, 0U, 0U) < 0) {
        detail = "BCryptCreateHash failed";
        return false;
    }
    UniqueHash hash(raw_hash);

    LARGE_INTEGER beginning{};
    beginning.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        detail = "SetFilePointerEx failed while hashing";
        return false;
    }

    // Keep the 1 MiB hashing buffer off the caller's stack. The DLL export may
    // be invoked from a host thread with the default 1 MiB Windows stack.
    std::vector<UCHAR> buffer(1024U * 1024U);
    std::uint64_t remaining = length;
    while (remaining > 0U) {
        const DWORD wanted = static_cast<DWORD>(
            remaining < buffer.size() ? remaining : buffer.size());
        DWORD read{};
        if (!ReadFile(file, buffer.data(), wanted, &read, nullptr) || read != wanted) {
            detail = "ReadFile failed while hashing";
            return false;
        }
        if (BCryptHashData(raw_hash, buffer.data(), read, 0U) < 0) {
            detail = "BCryptHashData failed";
            return false;
        }
        remaining -= read;
    }

    std::vector<std::byte> digest(hash_size);
    if (BCryptFinishHash(raw_hash, reinterpret_cast<PUCHAR>(digest.data()), hash_size, 0U) < 0) {
        detail = "BCryptFinishHash failed";
        return false;
    }
    digest_hex = HexLower(digest);
    return true;
}

} // namespace

FingerprintResult FingerprintExecutable(const std::filesystem::path& executable_path) noexcept {
    FingerprintResult result{};
    try {
        UniqueHandle file(CreateFileW(
            executable_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) {
            result.detail = "CreateFileW failed";
            return result;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
            result.detail = "GetFileSizeEx failed";
            return result;
        }
        result.fingerprint.file_size = static_cast<std::uint64_t>(size.QuadPart);

        IMAGE_DOS_HEADER dos{};
        if (!ReadExactly(file.get(), &dos, sizeof(dos), 0U) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
            result.detail = "invalid DOS header";
            return result;
        }

        const auto nt_offset = static_cast<std::uint64_t>(dos.e_lfanew);
        DWORD signature{};
        IMAGE_FILE_HEADER file_header{};
        if (!ReadExactly(file.get(), &signature, sizeof(signature), nt_offset) || signature != IMAGE_NT_SIGNATURE ||
            !ReadExactly(file.get(), &file_header, sizeof(file_header), nt_offset + sizeof(signature))) {
            result.detail = "invalid PE header";
            return result;
        }
        result.fingerprint.pe_timestamp = file_header.TimeDateStamp;
        result.fingerprint.machine = file_header.Machine;

        if (file_header.NumberOfSections == 0U || file_header.NumberOfSections > 96U) {
            result.detail = "invalid PE section count";
            return result;
        }
        const auto section_table_offset = nt_offset + sizeof(signature) +
            sizeof(file_header) + file_header.SizeOfOptionalHeader;
        const auto section_table_bytes =
            static_cast<std::uint64_t>(file_header.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        const auto file_size = result.fingerprint.file_size;
        if (section_table_offset > file_size ||
            section_table_bytes > file_size - section_table_offset) {
            result.detail = "PE section table extends past end of file";
            return result;
        }

        static constexpr std::array<unsigned char, IMAGE_SIZEOF_SHORT_NAME> kTextName{
            '.', 't', 'e', 'x', 't', 0, 0, 0,
        };
        IMAGE_SECTION_HEADER text_section{};
        bool text_found{};
        for (std::uint16_t index = 0U; index < file_header.NumberOfSections; ++index) {
            IMAGE_SECTION_HEADER section{};
            const auto section_offset = section_table_offset +
                static_cast<std::uint64_t>(index) * sizeof(section);
            if (!ReadExactly(file.get(), &section, sizeof(section), section_offset)) {
                result.detail = "failed to read PE section header";
                return result;
            }
            if (std::memcmp(section.Name, kTextName.data(), kTextName.size()) == 0) {
                if (text_found) {
                    result.detail = "multiple .text sections are unsupported";
                    return result;
                }
                text_section = section;
                text_found = true;
            }
        }
        if (!text_found || text_section.SizeOfRawData == 0U) {
            result.detail = "non-empty .text section not found";
            return result;
        }
        const auto text_offset = static_cast<std::uint64_t>(text_section.PointerToRawData);
        const auto text_size = static_cast<std::uint64_t>(text_section.SizeOfRawData);
        if (text_offset > file_size || text_size > file_size - text_offset) {
            result.detail = ".text raw data extends past end of file";
            return result;
        }
        result.fingerprint.text_raw_size = text_section.SizeOfRawData;
        if (!HashRange(file.get(), text_offset, text_size,
                result.fingerprint.text_sha256_hex, result.detail)) {
            result.detail = ".text " + result.detail;
            return result;
        }
        if (!HashRange(file.get(), 0U, file_size,
                result.fingerprint.sha256_hex, result.detail)) {
            result.detail = "full file " + result.detail;
            return result;
        }
        result.ok = true;
        result.detail = "ok";
        return result;
    } catch (...) {
        result.ok = false;
        result.detail = "unexpected fingerprint exception";
        return result;
    }
}

bool FingerprintsExactlyMatch(const BuildFingerprint& actual, const BuildFingerprint& expected) noexcept {
    return actual.file_size == expected.file_size &&
        actual.pe_timestamp == expected.pe_timestamp &&
        actual.machine == expected.machine &&
        actual.sha256_hex == expected.sha256_hex;
}

bool CodeFingerprintsExactlyMatch(
    const BuildFingerprint& actual,
    const BuildFingerprint& expected) noexcept {
    return !expected.text_sha256_hex.empty() &&
        actual.pe_timestamp == expected.pe_timestamp &&
        actual.machine == expected.machine &&
        actual.text_raw_size == expected.text_raw_size &&
        actual.text_sha256_hex == expected.text_sha256_hex;
}

BuildGateResult EvaluateBuild(
    const FingerprintResult& actual,
    const std::span<const SupportedBuild> supported_builds) noexcept {
    if (!actual.ok) {
        return {BuildGateCode::fingerprint_error, nullptr, actual.detail};
    }
    for (const auto& build : supported_builds) {
        if (FingerprintsExactlyMatch(actual.fingerprint, build.fingerprint)) {
            return {BuildGateCode::allowed, &build, "exact build fingerprint matched"};
        }
    }
    return {BuildGateCode::unsupported_build, nullptr,
        "no exact SHA-256/size/timestamp/machine profile; capture remains disabled"};
}

std::span<const SupportedBuild> EmbeddedSupportedBuilds() noexcept {
    static const std::array<SupportedBuild, 1U> builds{{
        {
            "S10-test-5f808545",
            {
                303584768ULL,
                0x6A8C4540U,
                0x8664U,
                "5f8085456a27109681403d8c57533e6999fbd0664752ff5f2856985b5fbbde71",
                232640000U,
                "e6c8c585dac9d35615210ed575c2ce3ba7eda9f8b72b27dabd5ce7f527d7736e",
            },
            {
                "gml_Object_Loot_Ground_obj_Create_0",
                "gml_Script_anon@1084@gml_Object_Loot_Ground_obj_Create_0",
                "gml_Script_GetRareDropAnnouncement",
            },
            "gml_Object_Loot_Ground_obj_Create_0",
            "gml_Script_anon@1084@gml_Object_Loot_Ground_obj_Create_0",
            "gml_Script_GetRareDropAnnouncement",
            0x1690U,
            0x58B0U,
            0x0710U,
            3,
            27,
            0U,
            1U,
            2U,
        },
    }};
    return builds;
}

} // namespace hsot

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(HSOT_BRIDGE_BUILD)
#define HSOT_API extern "C" __declspec(dllexport)
#else
#define HSOT_API extern "C" __declspec(dllimport)
#endif
#else
#define HSOT_API extern "C"
#endif

namespace hsot {

enum class BridgeValidationCode : std::uint32_t {
    validated = 0,
    invalid_argument = 1,
    host_path_unavailable = 2,
    fingerprint_failed = 3,
    unsupported_build = 4,
    required_name_missing = 5,
    required_name_ambiguous = 6,
    semantic_validation_failed = 7,
    internal_error = 8,
    transport_start_failed = 9,
    hook_start_failed = 10,
    already_started = 11,
};

} // namespace hsot

// Validation only. This function never installs a hook or writes process memory.
// If detail is non-null and detail_capacity is positive, it receives a null-terminated UTF-8 message.
HSOT_API std::uint32_t HSOT_BridgeProtocolVersion() noexcept;
HSOT_API hsot::BridgeValidationCode HSOT_BridgeValidateHost(
    char* detail,
    std::size_t detail_capacity) noexcept;

// Explicit opt-in entry points. Nothing calls these from DllMain and this project
// intentionally contains no launcher or injector.
HSOT_API hsot::BridgeValidationCode HSOT_BridgeStartSensor(
    char* detail,
    std::size_t detail_capacity) noexcept;
HSOT_API void HSOT_BridgeStopSensor() noexcept;
HSOT_API bool HSOT_BridgeSensorRunning() noexcept;

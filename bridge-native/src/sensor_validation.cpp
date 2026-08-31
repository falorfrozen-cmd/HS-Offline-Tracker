#include "sensor_validation.h"

#include "hsot/named_routine_resolver.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace hsot {
namespace {

bool HostExecutablePath(std::filesystem::path& path, std::string& detail) noexcept {
    try {
        std::vector<wchar_t> buffer(1024U);
        for (;;) {
            const auto copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0U) {
                detail = "GetModuleFileNameW failed";
                return false;
            }
            if (copied < buffer.size() - 1U) {
                path = std::wstring(buffer.data(), static_cast<std::size_t>(copied));
                return true;
            }
            if (buffer.size() >= 32768U) {
                detail = "host executable path is too long";
                return false;
            }
            buffer.resize(buffer.size() * 2U);
        }
    } catch (...) {
        detail = "failed to allocate host executable path buffer";
        return false;
    }
}

} // namespace

void CopyBridgeDetail(
    const std::string& value,
    char* destination,
    const std::size_t capacity) noexcept {
    if (destination == nullptr || capacity == 0U) {
        return;
    }
    const auto bytes = (std::min)(value.size(), capacity - 1U);
    std::memcpy(destination, value.data(), bytes);
    destination[bytes] = '\0';
}

HostSensorValidation ValidateCurrentHostForSensor() noexcept {
    HostSensorValidation validation{};
    try {
        std::filesystem::path host_path;
        if (!HostExecutablePath(host_path, validation.detail)) {
            validation.code = BridgeValidationCode::host_path_unavailable;
            return validation;
        }

        const auto fingerprint = FingerprintExecutable(host_path);
        const auto gate = EvaluateBuild(fingerprint, EmbeddedSupportedBuilds());
        if (gate.code == BuildGateCode::fingerprint_error) {
            validation.code = BridgeValidationCode::fingerprint_failed;
            validation.detail = gate.detail;
            return validation;
        }
        if (gate.code != BuildGateCode::allowed || gate.profile == nullptr) {
            validation.code = BridgeValidationCode::unsupported_build;
            validation.detail = gate.detail;
            return validation;
        }

        const auto module = GetModuleHandleW(nullptr);
        const auto resolver = CreateRegistrationTableResolver(module);
        if (!resolver) {
            validation.code = BridgeValidationCode::internal_error;
            validation.detail = "failed to create read-only name resolver";
            return validation;
        }

        const auto name_validation = ValidateRequiredNames(
            *resolver, std::span<const std::string>(gate.profile->required_routine_names));
        if (!name_validation.ok) {
            const auto& first = name_validation.failures.front();
            validation.code = first.code == ResolveCode::ambiguous
                ? BridgeValidationCode::required_name_ambiguous
                : BridgeValidationCode::required_name_missing;
            validation.detail = first.name + ": " + first.detail;
            return validation;
        }

        const auto route = ValidateDropRoute(module, *resolver, *gate.profile);
        if (!route.ok) {
            validation.code = BridgeValidationCode::semantic_validation_failed;
            validation.detail = route.detail;
            return validation;
        }

        validation.code = BridgeValidationCode::validated;
        validation.profile = gate.profile;
        validation.route = route.route;
        validation.detail = "exact build, names, direct caller, and read-only argument contract validated";
        return validation;
    } catch (...) {
        validation.code = BridgeValidationCode::internal_error;
        validation.detail = "unexpected host-validation exception";
        return validation;
    }
}

} // namespace hsot

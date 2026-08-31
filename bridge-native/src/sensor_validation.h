#pragma once

#include "hsot/bridge_export.h"
#include "hsot/build_fingerprint.h"
#include "hsot/route_validation.h"

#include <cstddef>
#include <string>

namespace hsot {

struct HostSensorValidation {
    BridgeValidationCode code{BridgeValidationCode::internal_error};
    const SupportedBuild* profile{};
    ValidatedDropRoute route;
    std::string detail;
};

[[nodiscard]] HostSensorValidation ValidateCurrentHostForSensor() noexcept;

void CopyBridgeDetail(
    const std::string& value,
    char* destination,
    std::size_t capacity) noexcept;

} // namespace hsot

#include "hsot/bridge_export.h"

#include "hsot/event_protocol.h"
#include "sensor_validation.h"

std::uint32_t HSOT_BridgeProtocolVersion() noexcept {
    return hsot::protocol::kProtocolVersion;
}

hsot::BridgeValidationCode HSOT_BridgeValidateHost(char* detail, const std::size_t detail_capacity) noexcept {
    const auto validation = hsot::ValidateCurrentHostForSensor();
    hsot::CopyBridgeDetail(validation.detail, detail, detail_capacity);
    return validation.code;
}

#include "hsot/event_protocol.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace hsot::protocol {
namespace {

void AppendEscaped(std::string& output, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(kHex[(character >> 4U) & 0x0FU]);
                output.push_back(kHex[character & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void AppendKey(std::string& output, std::string_view key) {
    AppendEscaped(output, key);
    output.push_back(':');
}

void AppendUnsigned(std::string& output, const std::uint64_t value) {
    output += std::to_string(value);
}

void AppendSigned(std::string& output, const std::int64_t value) {
    output += std::to_string(value);
}

void AppendDouble(std::string& output, const double value) {
    if (!std::isfinite(value)) {
        output += "null";
        return;
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    output += stream.str();
}

void AppendEnvelope(std::string& output, const EventEnvelope& envelope, std::string_view event_type) {
    AppendKey(output, "protocol");
    AppendEscaped(output, kProtocolName);
    output += ",";
    AppendKey(output, "v");
    AppendUnsigned(output, envelope.protocol_version);
    output += ",";
    AppendKey(output, "kind");
    AppendEscaped(output, event_type);
    output += ",";
    AppendKey(output, "sequence");
    AppendUnsigned(output, envelope.sequence);
    output += ",";
    AppendKey(output, "emitted_at_unix_ms");
    AppendUnsigned(output, envelope.emitted_at_unix_ms);
    output += ",";
    AppendKey(output, "process_id");
    AppendUnsigned(output, envelope.process_id);
    output += ",";
    AppendKey(output, "build_id");
    AppendEscaped(output, envelope.build_id);
}

} // namespace

const char* ToString(const BridgeState state) noexcept {
    switch (state) {
    case BridgeState::scaffold_ready: return "scaffold_ready";
    case BridgeState::blocked: return "blocked";
    case BridgeState::transport_ready: return "transport_ready";
    case BridgeState::transport_error: return "transport_error";
    }
    return "blocked";
}

std::string SerializeNdjson(const StatusEvent& event) {
    std::string output;
    output.reserve(320U);
    output.push_back('{');
    AppendEnvelope(output, event.envelope, "bridge_status");
    output += ",";
    AppendKey(output, "state");
    AppendEscaped(output, ToString(event.state));
    output += ",";
    AppendKey(output, "code");
    AppendEscaped(output, event.code);
    output += ",";
    AppendKey(output, "detail");
    AppendEscaped(output, event.detail);
    output += "}\n";
    return output;
}

std::string SerializeNdjson(const SessionDeltaEvent& event) {
    std::string output;
    output.reserve(384U);
    output.push_back('{');
    AppendEnvelope(output, event.envelope, "session_delta");
    output += ",";
    AppendKey(output, "gold");
    AppendSigned(output, event.gold);
    output += ",";
    AppendKey(output, "xp");
    AppendSigned(output, event.xp);
    output += ",";
    AppendKey(output, "kills");
    AppendSigned(output, event.kills);
    output += ",";
    AppendKey(output, "source");
    AppendEscaped(output, event.source);
    output += "}\n";
    return output;
}

std::string SerializeNdjson(const VitalsEvent& event) {
    std::string output;
    output.reserve(320U);
    output.push_back('{');
    AppendEnvelope(output, event.envelope, "vitals");
    output += ",";
    AppendKey(output, "magic_find");
    AppendDouble(output, event.magic_find);
    output += ",";
    AppendKey(output, "source");
    AppendEscaped(output, event.source);
    output += "}\n";
    return output;
}

std::string SerializeNdjson(const SensorDiagnosticEvent& event) {
    std::string output;
    output.reserve(640U);
    output.push_back('{');
    AppendEnvelope(output, event.envelope, "sensor_diagnostic");

    const auto append_unsigned = [&output](const std::string_view key, const std::uint64_t value) {
        output.push_back(',');
        AppendKey(output, key);
        AppendUnsigned(output, value);
    };
    append_unsigned("gold_calls", event.gold_calls);
    append_unsigned("gold_accepted", event.gold_accepted);
    append_unsigned("gold_rejected", event.gold_rejected);
    append_unsigned("gold_delta_total", event.gold_delta_total);
    append_unsigned("xp_calls", event.xp_calls);
    append_unsigned("xp_accepted", event.xp_accepted);
    append_unsigned("xp_rejected", event.xp_rejected);
    append_unsigned("xp_delta_total", event.xp_delta_total);
    append_unsigned("kill_candidate_calls", event.kill_candidate_calls);
    output += ",";
    AppendKey(output, "kill_emission_enabled");
    output += event.kill_emission_enabled ? "true" : "false";
    output += ",";
    AppendKey(output, "kill_route");
    AppendEscaped(output, event.kill_route);
    append_unsigned("drop_calls", event.drop_calls);
    append_unsigned("drop_accepted", event.drop_accepted);
    append_unsigned("drop_rejected", event.drop_rejected);
    append_unsigned("drop_queue_dropped", event.drop_queue_dropped);
    append_unsigned("drop_emitted", event.drop_emitted);
    output += ",";
    AppendKey(output, "drop_route");
    AppendEscaped(output, event.drop_route);
    output += "}\n";
    return output;
}

std::string SerializeNdjson(const GroundDropEvent& event) {
    std::string output;
    output.reserve(1024U + event.properties.size() * 96U);
    output.push_back('{');
    AppendEnvelope(output, event.envelope, "ground_drop");

    const auto append_string = [&output](std::string_view key, std::string_view value) {
        output.push_back(',');
        AppendKey(output, key);
        AppendEscaped(output, value);
    };
    const auto append_double = [&output](std::string_view key, const double value) {
        output.push_back(',');
        AppendKey(output, key);
        AppendDouble(output, value);
    };

    append_string("event_id", event.event_id);
    append_string("source", event.source);
    if (event.room_name) append_string("room", *event.room_name);
    if (event.x) append_double("x", *event.x);
    if (event.y) append_double("y", *event.y);

    output.push_back(',');
    AppendKey(output, "item");
    output.push_back('{');
    bool first_item_field = true;
    const auto item_separator = [&output, &first_item_field] {
        if (!first_item_field) output.push_back(',');
        first_item_field = false;
    };
    const auto item_string = [&output, &item_separator](std::string_view key, std::string_view value) {
        item_separator();
        AppendKey(output, key);
        AppendEscaped(output, value);
    };
    const auto item_signed = [&output, &item_separator](std::string_view key, const std::int64_t value) {
        item_separator();
        AppendKey(output, key);
        AppendSigned(output, value);
    };
    const auto item_double = [&output, &item_separator](std::string_view key, const double value) {
        item_separator();
        AppendKey(output, key);
        AppendDouble(output, value);
    };
    const auto item_bool = [&output, &item_separator](std::string_view key, const bool value) {
        item_separator();
        AppendKey(output, key);
        output += value ? "true" : "false";
    };

    if (event.item_name) item_string("name", *event.item_name);
    if (event.rarity_name) item_string("rarity", *event.rarity_name);
    else if (event.rarity_id) item_signed("rarity", *event.rarity_id);
    if (event.tier) item_signed("tier", *event.tier);
    if (event.item_type) item_signed("item_type", *event.item_type);
    if (event.item_id) item_signed("item_id", *event.item_id);
    if (event.weapon_type) item_signed("weapon_type", *event.weapon_type);
    if (event.seed) item_signed("seed", *event.seed);
    item_signed("amount", event.amount < 1 ? 1 : event.amount);
    if (event.magic_find) item_double("magic_find", *event.magic_find);
    if (event.magic_find_drop) item_bool("magic_find_drop", *event.magic_find_drop);
    if (event.unscaled) item_bool("unscaled", *event.unscaled);
    if (event.item_data_hash) item_string("hash", *event.item_data_hash);
    if (event.fingerprint) item_string("fingerprint", *event.fingerprint);
    if (event.drop_sound_asset) item_string("drop_sound_asset", *event.drop_sound_asset);

    item_separator();
    AppendKey(output, "properties");
    output.push_back('[');
    for (std::size_t index = 0; index < event.properties.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        const auto& property = event.properties[index];
        output.push_back('{');
        AppendKey(output, "key");
        AppendEscaped(output, property.key);
        output.push_back(',');
        AppendKey(output, "label");
        AppendEscaped(output, property.label);
        output.push_back(',');
        AppendKey(output, "value");
        AppendEscaped(output, property.value);
        output.push_back('}');
    }
    output += "]}}\n";
    return output;
}

} // namespace hsot::protocol

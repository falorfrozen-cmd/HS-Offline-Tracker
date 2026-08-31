#include "hsot/event_protocol.h"

#include <string>

#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    hsot::protocol::StatusEvent status{};
    status.envelope.sequence = 7U;
    status.envelope.emitted_at_unix_ms = 1234U;
    status.envelope.process_id = 99U;
    status.envelope.build_id = "test-build";
    status.state = hsot::protocol::BridgeState::blocked;
    status.code = "unknown_build";
    status.detail = "line one\n\"line two\"";

    const std::string status_line = hsot::protocol::SerializeNdjson(status);
    REQUIRE(!status_line.empty());
    REQUIRE(status_line.back() == '\n');
    REQUIRE(status_line.find('\n') == status_line.size() - 1U);
    REQUIRE(status_line.find("line one\\n\\\"line two\\\"") != std::string::npos);
    REQUIRE(status_line.find("\"protocol\":\"hs-offline-tracker/1\"") != std::string::npos);
    REQUIRE(status_line.find("\"kind\":\"bridge_status\"") != std::string::npos);
    REQUIRE(status_line.find("protocol_version") == std::string::npos);

    hsot::protocol::SessionDeltaEvent delta{};
    delta.envelope.sequence = 8U;
    delta.gold = 1250;
    delta.xp = 42000;
    delta.kills = 3;
    delta.source = "aurie_named_gml";
    const std::string delta_line = hsot::protocol::SerializeNdjson(delta);
    REQUIRE(delta_line.back() == '\n');
    REQUIRE(delta_line.find('\n') == delta_line.size() - 1U);
    REQUIRE(delta_line.find("\"kind\":\"session_delta\"") != std::string::npos);
    REQUIRE(delta_line.find("\"gold\":1250") != std::string::npos);
    REQUIRE(delta_line.find("\"xp\":42000") != std::string::npos);
    REQUIRE(delta_line.find("\"kills\":3") != std::string::npos);
    REQUIRE(delta_line.find("\"source\":\"aurie_named_gml\"") != std::string::npos);

    hsot::protocol::VitalsEvent vitals{};
    vitals.envelope.sequence = 9U;
    vitals.magic_find = 43470.0;
    vitals.source = "gml_Script_StatMagicFind";
    const std::string vitals_line = hsot::protocol::SerializeNdjson(vitals);
    REQUIRE(vitals_line.back() == '\n');
    REQUIRE(vitals_line.find('\n') == vitals_line.size() - 1U);
    REQUIRE(vitals_line.find("\"kind\":\"vitals\"") != std::string::npos);
    REQUIRE(vitals_line.find("\"magic_find\":43470") != std::string::npos);
    REQUIRE(vitals_line.find(
        "\"source\":\"gml_Script_StatMagicFind\"") != std::string::npos);

    hsot::protocol::SensorDiagnosticEvent diagnostic{};
    diagnostic.envelope.sequence = 9U;
    diagnostic.gold_calls = 2U;
    diagnostic.gold_accepted = 1U;
    diagnostic.gold_rejected = 1U;
    diagnostic.gold_delta_total = 1250U;
    diagnostic.xp_calls = 1U;
    diagnostic.xp_accepted = 1U;
    diagnostic.xp_delta_total = 42000U;
    diagnostic.kill_candidate_calls = 3U;
    diagnostic.kill_emission_enabled = true;
    diagnostic.kill_route = "gml_Script_EnemyAddStatistics";
    diagnostic.drop_calls = 9U;
    diagnostic.drop_accepted = 2U;
    diagnostic.drop_rejected = 7U;
    diagnostic.drop_queue_dropped = 0U;
    diagnostic.drop_emitted = 2U;
    diagnostic.drop_route = "gml_Script_GetRareDropAnnouncement";
    const std::string diagnostic_line = hsot::protocol::SerializeNdjson(diagnostic);
    REQUIRE(diagnostic_line.back() == '\n');
    REQUIRE(diagnostic_line.find('\n') == diagnostic_line.size() - 1U);
    REQUIRE(diagnostic_line.find("\"kind\":\"sensor_diagnostic\"") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"gold_calls\":2") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"gold_accepted\":1") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"gold_rejected\":1") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"gold_delta_total\":1250") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"xp_calls\":1") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"xp_accepted\":1") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"xp_rejected\":0") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"xp_delta_total\":42000") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"kill_candidate_calls\":3") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"kill_emission_enabled\":true") != std::string::npos);
    REQUIRE(diagnostic_line.find(
        "\"kill_route\":\"gml_Script_EnemyAddStatistics\"") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"drop_calls\":9") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"drop_accepted\":2") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"drop_rejected\":7") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"drop_queue_dropped\":0") != std::string::npos);
    REQUIRE(diagnostic_line.find("\"drop_emitted\":2") != std::string::npos);
    REQUIRE(diagnostic_line.find(
        "\"drop_route\":\"gml_Script_GetRareDropAnnouncement\"") != std::string::npos);

    hsot::protocol::GroundDropEvent drop{};
    drop.envelope.sequence = 10U;
    drop.event_id = "test-8";
    drop.source = "game_rare_announcement";
    drop.item_name = "Example \"Drop\"";
    drop.rarity_id = 7;
    drop.item_type = 12;
    drop.item_id = 44;
    drop.properties.push_back({"damage", "Damage", "+10\nunsafe newline"});

    const std::string drop_line = hsot::protocol::SerializeNdjson(drop);
    REQUIRE(drop_line.back() == '\n');
    REQUIRE(drop_line.find('\n') == drop_line.size() - 1U);
    REQUIRE(drop_line.find("\\\"Drop\\\"") != std::string::npos);
    REQUIRE(drop_line.find("+10\\nunsafe newline") != std::string::npos);
    REQUIRE(drop_line.find("\"kind\":\"ground_drop\"") != std::string::npos);
    REQUIRE(drop_line.find("\"item\":{\"name\":") != std::string::npos);
    REQUIRE(drop_line.find("\"rarity\":7") != std::string::npos);
    REQUIRE(drop_line.find("\"item_type\":12") != std::string::npos);
    REQUIRE(drop_line.find("\"item_id\":44") != std::string::npos);
    REQUIRE(drop_line.find("\"item_name\"") == std::string::npos);
    return 0;
}

#include <YYToolkit/YYTK_Shared.hpp>

#include "hsot/build_fingerprint.h"
#include "hsot/event_protocol.h"
#include "hsot/event_transport.h"
#include "hsot_aurie/counter_validation.h"
#include "hsot_aurie/producer_profile.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Aurie;
using namespace YYTK;

namespace {

constexpr std::string_view kEventSource = "aurie_named_gml";

YYTKInterface* g_yytk{};
std::unique_ptr<hsot::IEventTransport> g_transport;
std::mutex g_transport_mutex;
std::atomic_bool g_stopping{};
std::atomic_bool g_transport_stopped{};
std::atomic_uint64_t g_sequence{1U};
std::string g_build_id{"unverified"};
const hsot::aurie::ProducerProfile* g_active_profile{};
std::uintptr_t g_executable_base{};

PFUNC_YYGMLScript g_original_gold{};
PFUNC_YYGMLScript g_original_xp{};
PFUNC_YYGMLScript g_original_kill_candidate{};
PFUNC_YYGMLScript g_original_ground_init{};
PFUNC_YYGMLScript g_original_magic_find{};
PFUNC_YYGMLScript g_original_room_goto{};
PFUNC_YYGMLScript g_original_player_item_drop{};
PFUNC_YYGMLScript g_original_api_exchange{};

// Hook ids that were actually installed, in installation order, so a partial
// failure can be rolled back without guessing which routes exist on this build.
std::vector<std::string> g_installed_hook_ids;
std::mutex g_installed_hook_mutex;

struct MetricCounters {
    std::atomic_int64_t pending{};
};

MetricCounters g_gold;
MetricCounters g_xp;
MetricCounters g_kills;

constexpr std::size_t kDropSnapshotCapacity = 2048U;
// Bound each background cycle so an extreme loot burst cannot monopolize a CPU
// core. At roughly 60 cycles/s this still drains about 500 rare drops/second.
constexpr std::size_t kMaximumDropsPerPublishCycle = 8U;

// Adaptive builds have no reviewed caller allowlist for the ground-item route.
// Zone-state restores and API reconstructions recreate old ground items right
// after a room loads, so the first moments of a room are not counted as drops.
constexpr std::uint64_t kRoomLoadDropQuietMs = 1500U;

// Room for the longest item name the game shows ("Judge, Jury & Executioner"
// is 25 bytes) with margin for other languages; longer names are cut at a
// character boundary, never lost.
constexpr std::size_t kDropNameCapacity = 96U;

struct DropSnapshot {
    std::int64_t rarity{};
    std::int64_t item_type{};
    std::int64_t item_id{};
    // Which weapon: a sword and a bow share ids, the name table needs this.
    std::int64_t weapon_type{};
    // The name the game shows, so the journal can name an item the table
    // does not know (a newer game build) and never has to guess.
    char name[kDropNameCapacity]{};
};

// Copies a game string into the fixed-size slot without allocating: the drop
// routine must not touch the heap. A cut never lands inside a UTF-8 sequence.
void CopyDropName(const RValue& value, char (&slot)[kDropNameCapacity]) noexcept {
    slot[0] = '\0';
    try {
        const char* text = value.ToCString();
        if (!text) {
            return;
        }
        std::size_t n = 0;
        while (n + 1 < kDropNameCapacity && text[n] != '\0') {
            slot[n] = text[n];
            ++n;
        }
        if (text[n] != '\0') {
            while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0U) == 0x80U) {
                --n;
            }
        }
        slot[n] = '\0';
    } catch (...) {
        slot[0] = '\0';
    }
}

// Hooks can be reached from more than one game thread.  The tiny producer guard
// serializes only the fixed-size copy below; no allocation, JSON formatting or
// pipe I/O ever runs in the game's drop routine.
class DropSnapshotQueue final {
public:
    [[nodiscard]] bool TryPush(const DropSnapshot& snapshot) noexcept {
        if (producer_guard_.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto read = read_index_.load(std::memory_order_acquire);
        if (write - read >= kDropSnapshotCapacity) {
            producer_guard_.clear(std::memory_order_release);
            return false;
        }
        entries_[write % kDropSnapshotCapacity] = snapshot;
        write_index_.store(write + 1U, std::memory_order_release);
        producer_guard_.clear(std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(DropSnapshot& snapshot) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        snapshot = entries_[read % kDropSnapshotCapacity];
        read_index_.store(read + 1U, std::memory_order_release);
        return true;
    }

private:
    std::array<DropSnapshot, kDropSnapshotCapacity> entries_{};
    std::atomic_size_t read_index_{};
    std::atomic_size_t write_index_{};
    std::atomic_flag producer_guard_ = ATOMIC_FLAG_INIT;
};

DropSnapshotQueue g_drop_snapshots;
std::optional<DropSnapshot> g_retry_drop;
std::atomic_uint64_t g_drop_queue_dropped{};
std::mutex g_publish_wait_mutex;
std::condition_variable g_publish_wake;
std::thread g_publish_worker;

// ---- Zone / room sensing ---------------------------------------------------
//
// One fixed-size, latest-wins slot. A room change is rare (seconds apart at
// the very least) so a slot that is overwritten before the worker reads it
// loses nothing the player still cares about.
struct ZoneSnapshot {
    char room[64]{};
    char zone[48]{};
    std::uint8_t buffs[16]{};
    std::uint8_t debuffs[16]{};
    std::uint8_t buff_count{};
    std::uint8_t debuff_count{};
    // -1 unknown, 0 no, 1 yes
    std::int8_t satanic_here{-1};
    bool zone_known{};
};

ZoneSnapshot g_zone_slot;
std::atomic_flag g_zone_guard = ATOMIC_FLAG_INIT;
std::atomic_bool g_zone_dirty{};
std::atomic_uint64_t g_last_room_change_ms{};

// Magic find is read after the game's own StatMagicFind returns. Stored as raw
// double bits so the hook side stays a single atomic store.
std::atomic_uint64_t g_magic_find_bits{};
std::atomic_bool g_magic_find_dirty{};
std::atomic_bool g_magic_find_seen{};

// Player-initiated inventory discards and API/market reconstructions call the
// same ground-item routine as real loot. On exact builds the caller allowlist
// tells them apart; on adaptive builds the surrounding routine is hooked and
// marks its own thread while it runs.
thread_local int t_drop_suppress_depth{};

[[nodiscard]] std::uint64_t UnixMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

[[nodiscard]] std::uint64_t MonotonicMilliseconds() noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

[[nodiscard]] hsot::protocol::EventEnvelope MakeEnvelope() {
    hsot::protocol::EventEnvelope envelope{};
    envelope.sequence = g_sequence.fetch_add(1U, std::memory_order_relaxed);
    envelope.emitted_at_unix_ms = UnixMilliseconds();
    envelope.process_id = GetCurrentProcessId();
    envelope.build_id = g_build_id;
    return envelope;
}

[[nodiscard]] hsot::PublishResult PublishLine(const std::string& line) noexcept {
    std::scoped_lock lock(g_transport_mutex);
    if (!g_transport) {
        return hsot::PublishResult::not_started;
    }
    return g_transport->TryPublish(line);
}

// The console is the only other place the status goes, and the player never
// sees it. One small append-only file beside the tracker's own data answers
// "did the sensor load, and what did it decide" without a debugger.
void LogFile(const std::string& line) noexcept {
    try {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
        if (n == 0U || n >= MAX_PATH) {
            return;
        }
        std::wstring dir(buffer, n);
        dir += L"\\HS Offline Tracker";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\producer.log";
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamp[40]{};
        const int len = std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u pid=%lu ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            static_cast<unsigned long>(GetCurrentProcessId()));
        std::string text(stamp, len > 0 ? static_cast<std::size_t>(len) : 0U);
        text += line;
        text += "\r\n";
        DWORD written{};
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

void PublishStatus(
    const hsot::protocol::BridgeState state,
    const std::string_view code,
    const std::string_view detail) {
    hsot::protocol::StatusEvent event{};
    event.envelope = MakeEnvelope();
    event.state = state;
    event.code.assign(code);
    event.detail.assign(detail);
    LogFile(std::string(hsot::protocol::ToString(state)) + " " + event.code + ": " + event.detail);
    static_cast<void>(PublishLine(hsot::protocol::SerializeNdjson(event)));
}

[[nodiscard]] bool CurrentExecutablePath(std::wstring& path) noexcept {
    try {
        std::vector<wchar_t> buffer(1024U);
        for (;;) {
            const DWORD copied = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0U) {
                return false;
            }
            if (copied < buffer.size() - 1U) {
                path.assign(buffer.data(), copied);
                return true;
            }
            if (buffer.size() >= 32768U) {
                return false;
            }
            buffer.resize(buffer.size() * 2U);
        }
    } catch (...) {
        return false;
    }
}

// Picks the exact reviewed profile when the executable matches one byte for
// byte. Any other Hero Siege build gets the adaptive profile: every route is
// resolved by its GML name at load time and every value is validated at call
// time, so a game update changes nothing unless a routine is renamed.
[[nodiscard]] bool SelectProfile(std::string& detail, bool& exact) noexcept {
    exact = false;
    std::wstring executable_path;
    if (!CurrentExecutablePath(executable_path)) {
        detail = "GetModuleFileNameW failed";
        return false;
    }

    const auto actual = hsot::FingerprintExecutable(executable_path);
    if (!actual.ok) {
        detail = actual.detail;
        return false;
    }

    g_active_profile = nullptr;
    for (const auto& candidate : hsot::aurie::kSupportedProfiles) {
        const hsot::BuildFingerprint expected{
            candidate.reference_file_size,
            candidate.pe_timestamp,
            candidate.machine,
            std::string(candidate.known_full_sha256_hex.front()),
            candidate.text_raw_size,
            std::string(candidate.text_sha256_hex),
        };
        if (hsot::CodeFingerprintsExactlyMatch(actual.fingerprint, expected)) {
            g_active_profile = &candidate;
            break;
        }
    }

    const std::string fingerprint_text = "machine=" +
        std::to_string(actual.fingerprint.machine) + " timestamp=" +
        std::to_string(actual.fingerprint.pe_timestamp) + " text_size=" +
        std::to_string(actual.fingerprint.text_raw_size) + " text_sha256=" +
        actual.fingerprint.text_sha256_hex + " full_size=" +
        std::to_string(actual.fingerprint.file_size);

    if (!g_active_profile) {
        if (actual.fingerprint.machine != 0x8664U) {
            detail = "unsupported machine type; " + fingerprint_text;
            return false;
        }
        g_active_profile = &hsot::aurie::kAdaptiveProfile;
        char stamp[32]{};
        static_cast<void>(std::snprintf(stamp, sizeof(stamp), "adaptive-%08x",
            static_cast<unsigned>(actual.fingerprint.pe_timestamp)));
        g_build_id.assign(stamp);
        detail = "no exact reviewed profile; named routes resolved on this build; " + fingerprint_text;
        return true;
    }

    exact = true;
    const auto& profile = *g_active_profile;
    bool known_full_file = actual.fingerprint.file_size == profile.reference_file_size;
    if (known_full_file) {
        known_full_file = false;
        for (const auto known_hash : profile.known_full_sha256_hex) {
            if (!known_hash.empty() && actual.fingerprint.sha256_hex == known_hash) {
                known_full_file = true;
                break;
            }
        }
    }
    g_build_id.assign(profile.build_id);
    detail = "exact machine/timestamp/.text profile matched; full_file=";
    detail += known_full_file ? "known" : "unrecognized_diagnostic_only";
    detail += " size=" + std::to_string(actual.fingerprint.file_size) +
        " sha256=" + actual.fingerprint.sha256_hex;
    return true;
}

[[nodiscard]] bool IsExecutableAddress(const void* address) noexcept {
    if (!address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) == 0U ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] hsot::aurie::ObservedNumber ObserveNumber(const RValue& value) noexcept {
    hsot::aurie::ObservedNumber observed{};
    switch (value.m_Kind) {
    case VALUE_REAL:
        observed.kind = hsot::aurie::ObservedNumberKind::real;
        observed.real = value.m_Real;
        break;
    case VALUE_INT32:
        observed.kind = hsot::aurie::ObservedNumberKind::int32;
        observed.integer = value.m_i32;
        break;
    case VALUE_INT64:
        observed.kind = hsot::aurie::ObservedNumberKind::int64;
        observed.integer = value.m_i64;
        break;
    default:
        observed.kind = hsot::aurie::ObservedNumberKind::unsupported;
        break;
    }
    return observed;
}

[[nodiscard]] std::optional<std::int64_t> PositiveDeltaAt(
    RValue** arguments,
    const int index) noexcept {
    if (!arguments || !arguments[index]) {
        return std::nullopt;
    }
    return hsot::aurie::DecodePositiveIntegralDelta(ObserveNumber(*arguments[index]));
}

// `value_argument_index` below zero means "find it": the earned amount is the
// argument that decodes as a positive integral number. When two arguments
// qualify the second one wins, which is the layout every 2026 build has used;
// the legacy layout carried a non-numeric flag in that position and therefore
// never reaches the tie.
[[nodiscard]] std::optional<std::int64_t> CaptureDelta(
    const int argument_count,
    RValue** arguments,
    const int expected_argument_count,
    const int value_argument_index) noexcept {
    if (!arguments) {
        return std::nullopt;
    }
    if (value_argument_index >= 0) {
        if (argument_count != expected_argument_count ||
            value_argument_index >= argument_count) {
            return std::nullopt;
        }
        return PositiveDeltaAt(arguments, value_argument_index);
    }
    if (argument_count < 1 || argument_count > 4) {
        return std::nullopt;
    }
    std::optional<std::int64_t> chosen;
    for (int index = 0; index < argument_count; ++index) {
        if (const auto candidate = PositiveDeltaAt(arguments, index)) {
            chosen = candidate;
        }
    }
    return chosen;
}

[[nodiscard]] std::optional<std::int64_t> CaptureKillStatisticId(
    const int argument_count,
    RValue** arguments) noexcept {
    if (argument_count != 1 || !arguments || !arguments[0]) {
        return std::nullopt;
    }
    return hsot::aurie::DecodeNonNegativeIntegralId(ObserveNumber(*arguments[0]));
}

[[nodiscard]] std::optional<std::int64_t> CaptureNonNegativeValue(
    const RValue& value) noexcept {
    return hsot::aurie::DecodeNonNegativeIntegralId(ObserveNumber(value));
}

[[nodiscard]] bool TryGetInstanceMember(
    const RValue& instance,
    const char* member_name,
    RValue& result) noexcept {
    if (!g_yytk || !member_name) {
        return false;
    }
    switch (instance.m_Kind) {
    case VALUE_REAL:
    case VALUE_OBJECT:
    case VALUE_INT32:
    case VALUE_INT64:
    case VALUE_REF:
        break;
    default:
        return false;
    }

    try {
        // CallBuiltin() would execute @@GlobalScope@@ first and allocate another
        // argument vector; CallBuiltinEx() performs only the one getter we need.
        const auto status = g_yytk->CallBuiltinEx(
            result,
            "variable_instance_get",
            nullptr,
            nullptr,
            {instance, RValue(member_name)});
        if (!AurieSuccess(status)) {
            return false;
        }
        return result.m_Kind != VALUE_UNDEFINED && result.m_Kind != VALUE_UNSET;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool TryGetStructMember(
    RValue& object,
    const char* member_name,
    RValue& result) noexcept {
    if (!g_yytk || object.m_Kind != VALUE_OBJECT || !member_name) {
        return false;
    }
    try {
        // The runner exposes a direct, non-allocating struct lookup. Keep the
        // builtin only as a compatibility fallback.
        const auto& runner = g_yytk->GetRunnerInterface();
        if (runner.StructGetMember) {
            if (RValue* member = runner.StructGetMember(&object, member_name)) {
                result = *member;
                return result.m_Kind != VALUE_UNDEFINED && result.m_Kind != VALUE_UNSET;
            }
        }
        const auto status = g_yytk->CallBuiltinEx(
            result,
            "variable_struct_get",
            nullptr,
            nullptr,
            {object, RValue(member_name)});
        if (!AurieSuccess(status)) {
            return false;
        }
        return result.m_Kind != VALUE_UNDEFINED && result.m_Kind != VALUE_UNSET;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool TryGetGlobal(const char* name, RValue& result) noexcept {
    if (!g_yytk || !name) {
        return false;
    }
    try {
        const auto status = g_yytk->CallBuiltinEx(
            result, "variable_global_get", nullptr, nullptr, {RValue(name)});
        if (!AurieSuccess(status)) {
            return false;
        }
        return result.m_Kind != VALUE_UNDEFINED && result.m_Kind != VALUE_UNSET;
    } catch (...) {
        return false;
    }
}

// The satanic zone candidates and the rolled modifiers are instance variables
// of the game's Controller_obj (its Create event fills them offline), not
// globals. One instance exists per session; the first one is the one.
[[nodiscard]] bool RValueIsTruthy(const RValue& value) noexcept;

// Rooms, objects and instances reach a hook as REAL ids on old runners and as
// VALUE_REF on current ones. Both are handed straight back to the runner.
[[nodiscard]] bool IsInstanceLike(const RValue& value) noexcept {
    switch (value.m_Kind) {
    case VALUE_REAL:
    case VALUE_INT32:
    case VALUE_INT64:
    case VALUE_REF:
    case VALUE_OBJECT:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool TryGetControllerMember(const char* name, RValue& result) noexcept {
    if (!g_yytk || !name) {
        return false;
    }
    try {
        RValue object_index;
        if (!AurieSuccess(g_yytk->CallBuiltinEx(object_index, "asset_get_index", nullptr, nullptr,
                {RValue("Controller_obj")}))) {
            return false;
        }
        // The runner answers with a reference, not a number; hand it back as is.
        if (!IsInstanceLike(object_index)) {
            return false;
        }
        RValue instance;
        if (!AurieSuccess(g_yytk->CallBuiltinEx(instance, "instance_find", nullptr, nullptr,
                {object_index, RValue(0.0)}))) {
            return false;
        }
        if (instance.m_Kind == VALUE_UNDEFINED || instance.m_Kind == VALUE_UNSET) {
            return false;
        }
        // The menus have no Controller_obj: instance_find answers noone (-4)
        // and variable_instance_get on that is a runtime error, not a miss.
        RValue exists_answer;
        if (!AurieSuccess(g_yytk->CallBuiltinEx(exists_answer, "instance_exists", nullptr, nullptr,
                {instance})) || !RValueIsTruthy(exists_answer)) {
            return false;
        }
        return TryGetInstanceMember(instance, name, result);
    } catch (...) {
        return false;
    }
}

// Controller_obj first, the global namespace as a fallback for older builds.
[[nodiscard]] bool TryGetZoneVariable(const char* name, RValue& result) noexcept {
    if (TryGetControllerMember(name, result)) {
        return true;
    }
    return TryGetGlobal(name, result);
}

// Copies the small non-negative integers of a GML array into a byte list.
[[nodiscard]] std::uint8_t CopyByteArray(
    RValue& array,
    std::uint8_t* out,
    const std::uint8_t capacity) noexcept {
    if (!g_yytk || array.m_Kind != VALUE_ARRAY) {
        return 0U;
    }
    std::size_t size{};
    if (!AurieSuccess(g_yytk->GetArraySize(array, size))) {
        return 0U;
    }
    std::uint8_t count{};
    for (std::size_t index = 0U; index < size && count < capacity; ++index) {
        RValue* element{};
        if (!AurieSuccess(g_yytk->GetArrayEntry(array, index, element)) || !element) {
            break;
        }
        const auto id = CaptureNonNegativeValue(*element);
        if (!id || *id > 255) {
            continue;
        }
        out[count++] = static_cast<std::uint8_t>(*id);
    }
    return count;
}

void CopyText(char* destination, const std::size_t capacity, const std::string& text) noexcept {
    if (capacity == 0U) {
        return;
    }
    const std::size_t length = text.size() < capacity - 1U ? text.size() : capacity - 1U;
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

[[nodiscard]] bool RoomNameOf(const RValue& room, std::string& name) noexcept {
    if (!g_yytk) {
        return false;
    }
    try {
        RValue result;
        const auto status = g_yytk->CallBuiltinEx(
            result, "room_get_name", nullptr, nullptr, {room});
        if (!AurieSuccess(status) || result.m_Kind != VALUE_STRING) {
            return false;
        }
        name = result.ToString();
        return !name.empty();
    } catch (...) {
        return false;
    }
}

// "Act_09_03" -> "Satanic_9_3", the spelling the tracker's zone table uses.
[[nodiscard]] std::string SatanicZoneCode(const std::string& room_name) {
    if (room_name.rfind("Act_", 0U) != 0U) {
        return {};
    }
    const auto second = room_name.find('_', 4U);
    if (second == std::string::npos) {
        return {};
    }
    const std::string act = room_name.substr(4U, second - 4U);
    const std::string zone = room_name.substr(second + 1U);
    if (act.empty() || zone.empty()) {
        return {};
    }
    for (const char c : act + zone) {
        if (c < '0' || c > '9') {
            return {};
        }
    }
    return "Satanic_" + std::to_string(std::stoi(act)) + "_" + std::to_string(std::stoi(zone));
}

[[nodiscard]] bool RValueIsTruthy(const RValue& value) noexcept {
    switch (value.m_Kind) {
    case VALUE_BOOL:
        return value.m_i32 != 0;
    case VALUE_REAL:
        return value.m_Real != 0.0;
    case VALUE_INT32:
        return value.m_i32 != 0;
    case VALUE_INT64:
        return value.m_i64 != 0;
    case VALUE_OBJECT:
    case VALUE_ARRAY:
    case VALUE_STRING:
        return true;
    default:
        return false;
    }
}

// Asks the game which of its own candidate rooms is the active satanic zone.
// LoadSatanicZone(room) answers for the room it is given, so every candidate
// in global.satanicZone is tried once. An answer that is "yes" for every room
// is not an answer and leaves the zone unknown rather than wrong.
// "Act_09_03" and nothing longer: "Act_02_02_02" is a sub-area, not a zone.
[[nodiscard]] bool IsActZoneRoomName(const std::string& name) noexcept {
    if (name.size() != 9U || name.rfind("Act_", 0U) != 0U || name[6] != '_') {
        return false;
    }
    for (const std::size_t i : {4U, 5U, 7U, 8U}) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

// Which room the game itself calls the active satanic zone. Offline the game
// rolls it in GetSatanicZoneOffline and keeps the answer in a protected value
// (the anti-tamper store) whose handle is Controller_obj.satanicZone; the
// game's own GPV(handle) reads it back. Nothing here depends on room ids or
// on where the character stands.
std::string g_zone_diag;

[[nodiscard]] bool ResolveSatanicZone(
    CInstance* self,
    CInstance* other,
    const std::string& target_room_name,
    ZoneSnapshot& snapshot) noexcept {
    if (!g_yytk) {
        return false;
    }
    try {
        RValue handle;
        if (!TryGetZoneVariable("satanicZone", handle)) {
            g_zone_diag = "no satanicZone on Controller_obj";
            return false;
        }
        RValue zone_room;
        if (handle.m_Kind == VALUE_ARRAY) {
            // An older layout kept the candidates in an array; the roll is
            // then the first entry.
            RValue* first{};
            if (!AurieSuccess(g_yytk->GetArrayEntry(handle, 0U, first)) || !first) {
                g_zone_diag = "empty satanicZone array";
                return false;
            }
            zone_room = *first;
        } else {
            if (!IsInstanceLike(handle)) {
                g_zone_diag = "satanicZone kind=" + std::to_string(static_cast<int>(handle.m_Kind));
                return false;
            }
            const auto status = g_yytk->CallGameScriptEx(
                zone_room, "gml_Script_GPV", self, other, {handle});
            if (!AurieSuccess(status)) {
                g_zone_diag = "GPV call failed status=" + std::to_string(static_cast<int>(status));
                return false;
            }
        }
        if (!IsInstanceLike(zone_room)) {
            g_zone_diag = "GPV kind=" + std::to_string(static_cast<int>(zone_room.m_Kind));
            return false;
        }
        // -1 / noone: no zone rolled (the difficulty has none, or not yet).
        if (zone_room.m_Kind == VALUE_REAL && zone_room.m_Real < 0.0) {
            g_zone_diag = "no zone rolled";
            return false;
        }
        std::string chosen_room;
        if (!RoomNameOf(zone_room, chosen_room)) {
            g_zone_diag = "room name lookup failed";
            return false;
        }
        const std::string zone_code = SatanicZoneCode(chosen_room);
        if (zone_code.empty()) {
            g_zone_diag = "not an act room: " + chosen_room;
            return false;
        }
        CopyText(snapshot.zone, sizeof(snapshot.zone), zone_code);
        snapshot.zone_known = true;
        snapshot.satanic_here = (chosen_room == target_room_name) ? 1 : 0;
        g_zone_diag = "zone " + chosen_room;
        return true;
    } catch (...) {
        g_zone_diag = "exception";
        return false;
    }
}

void CaptureZone(CInstance* self, CInstance* other, const RValue& target_room) noexcept {
    ZoneSnapshot snapshot{};
    std::string room_name;
    if (!RoomNameOf(target_room, room_name)) {
        return;
    }
    CopyText(snapshot.room, sizeof(snapshot.room), room_name);

    RValue buffs;
    if (TryGetZoneVariable("satanicZoneBuff", buffs)) {
        snapshot.buff_count = CopyByteArray(buffs, snapshot.buffs, sizeof(snapshot.buffs));
    }
    RValue debuffs;
    if (TryGetZoneVariable("satanicZoneDebuff", debuffs)) {
        snapshot.debuff_count = CopyByteArray(debuffs, snapshot.debuffs, sizeof(snapshot.debuffs));
    }
    if (!ResolveSatanicZone(self, other, room_name, snapshot)) {
        static std::atomic_int logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
            LogFile(std::string("satanic zone unresolved for room ") + snapshot.room +
                " (buffs=" + std::to_string(snapshot.buff_count) +
                " debuffs=" + std::to_string(snapshot.debuff_count) + ") " + g_zone_diag);
        }
    }

    // Latest wins; a hook never waits for the worker.
    if (g_zone_guard.test_and_set(std::memory_order_acquire)) {
        return;
    }
    g_zone_slot = snapshot;
    g_zone_guard.clear(std::memory_order_release);
    g_zone_dirty.store(true, std::memory_order_release);
}

[[nodiscard]] std::optional<DropSnapshot> CaptureGroundItem(
    const RValue& ground) noexcept {
    switch (ground.m_Kind) {
    case VALUE_REAL:
    case VALUE_OBJECT:
    case VALUE_INT32:
    case VALUE_INT64:
    case VALUE_REF:
        break;
    default:
        return std::nullopt;
    }

    // LootGroundInit arg0 is a runner instance reference/ID. The finalized
    // item itself is a struct stored in ground.itemInstance.
    RValue item;
    if (!TryGetInstanceMember(ground, "itemInstance", item)) {
        return std::nullopt;
    }
    if (item.m_Kind != VALUE_OBJECT) {
        return std::nullopt;
    }

    // Item payloads are GameMaker structs. In particular, itemInfoStruct uses
    // numeric-string keys such as "27".
    RValue info;
    if (!TryGetStructMember(item, "itemInfoStruct", info)) {
        return std::nullopt;
    }

    RValue rarity_value;
    if (!TryGetStructMember(info, "27", rarity_value)) {
        return std::nullopt;
    }

    const auto rarity = CaptureNonNegativeValue(rarity_value);
    if (!rarity) {
        return std::nullopt;
    }
    if (!hsot::aurie::IsTrackedRareRarity(*rarity)) {
        return std::nullopt;
    }

    // Most ground items stop above. Only tracked rarities pay for the remaining
    // identity fields needed by the journal.
    RValue item_type_value;
    RValue definition;
    if (!TryGetStructMember(item, "itemType", item_type_value) ||
        !TryGetStructMember(item, "itemDefinitionStruct", definition)) {
        return std::nullopt;
    }
    RValue item_id_value;
    if (!TryGetStructMember(definition, "b", item_id_value)) {
        return std::nullopt;
    }
    const auto item_type = CaptureNonNegativeValue(item_type_value);
    const auto item_id = CaptureNonNegativeValue(item_id_value);
    if (!item_type || !item_id) {
        return std::nullopt;
    }
    if (*item_type > 255 || *item_id > 65535) {
        return std::nullopt;
    }
    DropSnapshot snapshot{*rarity, *item_type, *item_id};
    // The definition's "j" is the weapon type (the save parser reads it from
    // the same place); "28" of the info struct is the display name, already
    // localized once the item exists on the ground.
    RValue weapon_type_value;
    if (TryGetStructMember(definition, "j", weapon_type_value)) {
        if (const auto weapon_type = CaptureNonNegativeValue(weapon_type_value);
            weapon_type && *weapon_type <= 255) {
            snapshot.weapon_type = *weapon_type;
        }
    }
    RValue name_value;
    if (TryGetStructMember(info, "28", name_value) && name_value.m_Kind == VALUE_STRING) {
        CopyDropName(name_value, snapshot.name);
    }
    return snapshot;
}

[[nodiscard]] bool AtomicAccumulate(
    std::atomic_int64_t& accumulator,
    const std::int64_t delta) noexcept {
    auto current = accumulator.load(std::memory_order_relaxed);
    for (;;) {
        std::int64_t next{};
        if (!hsot::aurie::AddPositiveWithoutOverflow(current, delta, next)) {
            return false;
        }
        if (accumulator.compare_exchange_weak(
                current, next, std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void RecordMetric(MetricCounters& counters, const std::optional<std::int64_t> delta) noexcept {
    if (delta) {
        static_cast<void>(AtomicAccumulate(counters.pending, *delta));
    }
}

RValue& HookGold(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    const auto& route = g_active_profile->routes[0];
    const auto delta = CaptureDelta(
        argument_count, arguments, route.expected_argument_count,
        route.value_argument_index);
    if (!g_original_gold) {
        return result;
    }
    RValue& original_result = g_original_gold(self, other, result, argument_count, arguments);
    if (!g_stopping.load(std::memory_order_acquire)) {
        RecordMetric(g_gold, delta);
    }
    return original_result;
}

RValue& HookXp(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    const auto& route = g_active_profile->routes[1];
    const auto delta = CaptureDelta(
        argument_count, arguments, route.expected_argument_count,
        route.value_argument_index);
    if (!g_original_xp) {
        return result;
    }
    RValue& original_result = g_original_xp(self, other, result, argument_count, arguments);
    const bool rejected = hsot::aurie::IsExplicitFalseBooleanResult(
        static_cast<std::uint32_t>(original_result.m_Kind), original_result.m_i32);
    if (!g_stopping.load(std::memory_order_acquire)) {
        RecordMetric(g_xp, rejected ? std::nullopt : delta);
    }
    return original_result;
}

RValue& HookKillCandidate(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    const auto statistic_id = CaptureKillStatisticId(argument_count, arguments);
    if (!g_original_kill_candidate) {
        return result;
    }
    RValue& original_result =
        g_original_kill_candidate(self, other, result, argument_count, arguments);
    if (statistic_id && !g_stopping.load(std::memory_order_acquire)) {
        // Live validation on the packaged tracker observed 56 accepted calls
        // in the same save interval in which statisticTotalMonsterKills grew
        // by exactly 56. Count after the original returns, just like the gold
        // and XP routes, and queue one earned kill per validated call.
        RecordMetric(g_kills, std::int64_t{1});
    }
    return original_result;
}

[[nodiscard]] bool GroundCallAccepted(const std::uint32_t caller_rva) noexcept {
    if (!g_active_profile) {
        return false;
    }
    const auto& drop = g_active_profile->drop_route;
    if (!drop.enabled) {
        return false;
    }
    if (drop.caller_allowlist) {
        return hsot::aurie::IsGroundItemCreateCallerRva(drop, caller_rva);
    }
    if (t_drop_suppress_depth > 0) {
        return false;
    }
    const auto last_room = g_last_room_change_ms.load(std::memory_order_acquire);
    if (last_room != 0U && MonotonicMilliseconds() - last_room < kRoomLoadDropQuietMs) {
        return false;
    }
    return true;
}

RValue& HookGroundInit(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    const auto caller_address = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto caller_rva = caller_address >= g_executable_base &&
            caller_address - g_executable_base <= std::numeric_limits<std::uint32_t>::max()
        ? static_cast<std::uint32_t>(caller_address - g_executable_base)
        : 0U;
    const bool accepted = GroundCallAccepted(caller_rva);

    if (!g_original_ground_init) {
        return result;
    }

    // Original-first is deliberate. LootGroundInit is allowed to complete every
    // native placement side effect before we read the engine-owned item struct.
    RValue& original_result =
        g_original_ground_init(self, other, result, argument_count, arguments);
    if (!accepted) {
        return original_result;
    }

    if (g_stopping.load(std::memory_order_acquire)) {
        return original_result;
    }

    std::optional<DropSnapshot> snapshot;
    if (argument_count >= 1 && arguments && arguments[0]) {
        // arg0 is the newly created Loot_Ground instance. Its finalized item is
        // ground.itemInstance; later arguments are caller options and must
        // never be decoded as an item.
        snapshot = CaptureGroundItem(*arguments[0]);
    }
    if (!snapshot) {
        return original_result;
    }

    if (!g_drop_snapshots.TryPush(*snapshot)) {
        g_drop_queue_dropped.fetch_add(1U, std::memory_order_relaxed);
    }
    return original_result;
}

// Magic find probe: the four elements StatMagicFind returns plus two of its
// arguments, stored as raw double bits by the hook and written to producer.log
// by the worker. Element 0 is what the game scales (StatForge notes); which
// element the character sheet prints is settled from these lines.
struct MagicFindProbe {
    std::atomic_uint64_t element[4]{};
    std::atomic_int count{};
    std::atomic_uint64_t arg5{};
    std::atomic_uint64_t arg7{};
    std::atomic_int calls{};
};
MagicFindProbe g_mf_probe;
std::atomic_int g_mf_probe_logged{};

[[nodiscard]] bool NumberBits(const RValue& v, std::uint64_t& bits) noexcept {
    switch (v.m_Kind) {
    case VALUE_REAL:
    case VALUE_INT32:
    case VALUE_INT64:
        bits = std::bit_cast<std::uint64_t>(v.ToDouble());
        return true;
    default:
        return false;
    }
}

RValue& HookMagicFind(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    if (!g_original_magic_find) {
        return result;
    }
    std::uint64_t a5{}, a7{};
    const bool have5 = argument_count > 5 && arguments && arguments[5] && NumberBits(*arguments[5], a5);
    const bool have7 = argument_count > 7 && arguments && arguments[7] && NumberBits(*arguments[7], a7);
    RValue& original_result =
        g_original_magic_find(self, other, result, argument_count, arguments);
    if (g_stopping.load(std::memory_order_acquire)) {
        return original_result;
    }
    if (have5) g_mf_probe.arg5.store(a5, std::memory_order_relaxed);
    if (have7) g_mf_probe.arg7.store(a7, std::memory_order_relaxed);
    int count = 0;
    if (original_result.m_Kind == VALUE_ARRAY && g_yytk) {
        std::size_t n{};
        if (AurieSuccess(g_yytk->GetArraySize(original_result, n))) {
            for (std::size_t i = 0; i < n && i < 4U; ++i) {
                RValue* e{};
                std::uint64_t bits{};
                if (!AurieSuccess(g_yytk->GetArrayEntry(original_result, i, e)) || !e || !NumberBits(*e, bits)) {
                    break;
                }
                g_mf_probe.element[i].store(bits, std::memory_order_relaxed);
                count = static_cast<int>(i) + 1;
            }
        }
    } else {
        std::uint64_t bits{};
        if (NumberBits(original_result, bits)) {
            g_mf_probe.element[0].store(bits, std::memory_order_relaxed);
            count = 1;
        }
    }
    if (count > 0) {
        g_mf_probe.count.store(count, std::memory_order_relaxed);
        g_mf_probe.calls.fetch_add(1, std::memory_order_relaxed);
        // Element 0 is the value the game itself scales; publish it as the
        // live magic find until the sheet element is settled.
        const auto bits = g_mf_probe.element[0].load(std::memory_order_relaxed);
        const auto previous = g_magic_find_bits.exchange(bits, std::memory_order_acq_rel);
        if (previous != bits || !g_magic_find_seen.load(std::memory_order_relaxed)) {
            g_magic_find_seen.store(true, std::memory_order_relaxed);
            g_magic_find_dirty.store(true, std::memory_order_release);
        }
    }
    return original_result;
}

RValue& HookRoomGoto(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    if (!g_original_room_goto) {
        return result;
    }
    RValue& original_result =
        g_original_room_goto(self, other, result, argument_count, arguments);
    if (g_stopping.load(std::memory_order_acquire)) {
        return original_result;
    }
    g_last_room_change_ms.store(MonotonicMilliseconds(), std::memory_order_release);
    if (argument_count >= 1 && arguments && arguments[0]) {
        // A room change is a rare event on a routine that already allocates
        // freely; the string work below is the one place this module allows it.
        CaptureZone(self, other, *arguments[0]);
    }
    return original_result;
}

RValue& HookPlayerItemDrop(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    if (!g_original_player_item_drop) {
        return result;
    }
    ++t_drop_suppress_depth;
    RValue& original_result =
        g_original_player_item_drop(self, other, result, argument_count, arguments);
    --t_drop_suppress_depth;
    return original_result;
}

RValue& HookApiExchange(
    CInstance* self,
    CInstance* other,
    RValue& result,
    const int argument_count,
    RValue** arguments) {
    if (!g_original_api_exchange) {
        return result;
    }
    ++t_drop_suppress_depth;
    RValue& original_result =
        g_original_api_exchange(self, other, result, argument_count, arguments);
    --t_drop_suppress_depth;
    return original_result;
}

struct HookBinding {
    std::string_view routine_name;
    std::string_view hook_id;
    PVOID detour{};
    PFUNC_YYGMLScript* original{};
    // A route the producer can live without: a rename on a future build
    // downgrades it instead of blocking every other sensor.
    bool optional{};
};

[[nodiscard]] bool ResolveAndInstallHook(
    const HookBinding& binding,
    std::string& detail) noexcept {
    PVOID named_pointer{};
    const std::string routine_name(binding.routine_name);
    const auto resolve_status = g_yytk->GetNamedRoutinePointer(routine_name.c_str(), &named_pointer);
    if (!AurieSuccess(resolve_status) || !named_pointer) {
        detail = routine_name + " lookup failed status=" +
            std::to_string(static_cast<int>(resolve_status));
        return false;
    }

    const auto* script = static_cast<const CScript*>(named_pointer);
    if (!script->m_Functions || !script->m_Functions->m_ScriptFunction) {
        detail = routine_name + " has no YYC script function";
        return false;
    }
    PVOID source = reinterpret_cast<PVOID>(script->m_Functions->m_ScriptFunction);
    if (!IsExecutableAddress(source)) {
        detail = routine_name + " resolved outside executable memory";
        return false;
    }

    PVOID trampoline{};
    const std::string hook_id(binding.hook_id);
    const auto hook_status = MmCreateHook(
        g_ArSelfModule, hook_id.c_str(), source, binding.detour, &trampoline);
    if (!AurieSuccess(hook_status) || !trampoline) {
        detail = routine_name + " hook failed status=" +
            std::to_string(static_cast<int>(hook_status));
        return false;
    }
    *binding.original = reinterpret_cast<PFUNC_YYGMLScript>(trampoline);
    try {
        std::scoped_lock lock(g_installed_hook_mutex);
        g_installed_hook_ids.push_back(hook_id);
    } catch (...) {
        // The hook is live either way; a lost bookkeeping entry only affects
        // rollback on a later failure.
    }
    return true;
}

void RemoveInstalledHooks() noexcept {
    std::vector<std::string> ids;
    try {
        std::scoped_lock lock(g_installed_hook_mutex);
        ids.swap(g_installed_hook_ids);
    } catch (...) {
        return;
    }
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const auto status = MmRemoveHook(g_ArSelfModule, it->c_str());
        if (!AurieSuccess(status) && g_yytk) {
            g_yytk->Print(CM_LIGHTRED,
                "[HS Offline Tracker] hook rollback failed for %s status=%d; keeping passthrough trampoline",
                it->c_str(), static_cast<int>(status));
        }
    }
    g_original_gold = nullptr;
    g_original_xp = nullptr;
    g_original_kill_candidate = nullptr;
    g_original_ground_init = nullptr;
    g_original_magic_find = nullptr;
    g_original_room_goto = nullptr;
    g_original_player_item_drop = nullptr;
    g_original_api_exchange = nullptr;
}

struct InstallReport {
    bool drops{};
    bool magic_find{};
    bool room{};
    bool player_drop_guard{};
    bool api_guard{};
    std::string missing;
};

[[nodiscard]] bool InstallNamedHooks(std::string& detail, InstallReport& report) noexcept {
    if (!g_active_profile) {
        detail = "no producer profile is active";
        return false;
    }
    const auto& routes = g_active_profile->routes;
    const auto& drop = g_active_profile->drop_route;
    const auto& magic = g_active_profile->magic_find_route;
    const auto& room = g_active_profile->room_route;

    std::array<HookBinding, 8U> bindings{};
    std::size_t binding_count{};
    bindings[binding_count++] = HookBinding{routes[0].routine_name, routes[0].hook_id,
        reinterpret_cast<PVOID>(&HookGold), &g_original_gold, false};
    bindings[binding_count++] = HookBinding{routes[1].routine_name, routes[1].hook_id,
        reinterpret_cast<PVOID>(&HookXp), &g_original_xp, false};
    bindings[binding_count++] = HookBinding{routes[2].routine_name, routes[2].hook_id,
        reinterpret_cast<PVOID>(&HookKillCandidate), &g_original_kill_candidate, false};
    if (drop.enabled) {
        bindings[binding_count++] = HookBinding{drop.routine_name, drop.hook_id,
            reinterpret_cast<PVOID>(&HookGroundInit), &g_original_ground_init, true};
    }
    if (magic.enabled) {
        bindings[binding_count++] = HookBinding{magic.routine_name, magic.hook_id,
            reinterpret_cast<PVOID>(&HookMagicFind), &g_original_magic_find, true};
    }
    if (room.enabled) {
        bindings[binding_count++] = HookBinding{room.routine_name, room.hook_id,
            reinterpret_cast<PVOID>(&HookRoomGoto), &g_original_room_goto, true};
    }
    if (drop.enabled && !drop.caller_allowlist) {
        bindings[binding_count++] = HookBinding{"gml_Script_CA_playerItemDrop",
            "hsot_guard_player_item_drop", reinterpret_cast<PVOID>(&HookPlayerItemDrop),
            &g_original_player_item_drop, true};
        bindings[binding_count++] = HookBinding{"gml_Object_Api_Exchange_Client_obj_Other_68",
            "hsot_guard_api_exchange", reinterpret_cast<PVOID>(&HookApiExchange),
            &g_original_api_exchange, true};
    }

    for (std::size_t index = 0U; index < binding_count; ++index) {
        std::string hook_detail;
        if (ResolveAndInstallHook(bindings[index], hook_detail)) {
            continue;
        }
        if (!bindings[index].optional) {
            detail = hook_detail;
            g_stopping.store(true, std::memory_order_release);
            RemoveInstalledHooks();
            return false;
        }
        if (!report.missing.empty()) {
            report.missing += "; ";
        }
        report.missing += hook_detail;
    }
    report.drops = g_original_ground_init != nullptr;
    report.magic_find = g_original_magic_find != nullptr;
    report.room = g_original_room_goto != nullptr;
    report.player_drop_guard = g_original_player_item_drop != nullptr;
    report.api_guard = g_original_api_exchange != nullptr;

    // On an adaptive build the guards stand in for the caller allowlist. If the
    // player-discard guard is missing, a dropped item from the inventory would
    // be journalled as loot; that route is switched off rather than left wrong.
    if (report.drops && !drop.caller_allowlist && !report.player_drop_guard) {
        static_cast<void>(MmRemoveHook(g_ArSelfModule, std::string(drop.hook_id).c_str()));
        g_original_ground_init = nullptr;
        report.drops = false;
        if (!report.missing.empty()) {
            report.missing += "; ";
        }
        report.missing += "ground drops disabled: player discard guard unavailable";
    }

    detail = "Gold, XP and validated kill routes enabled";
    detail += report.drops ? "; ground-item route enabled" : "; rare drops unavailable on this build";
    detail += report.magic_find ? "; magic find enabled" : "; magic find unavailable";
    detail += report.room ? "; room and satanic zone enabled" : "; room sensing unavailable";
    if (!report.missing.empty()) {
        detail += " [" + report.missing + "]";
    }
    return true;
}

void RequeueDelta(
    MetricCounters& counters,
    const std::int64_t delta) noexcept {
    if (delta > 0) {
        static_cast<void>(AtomicAccumulate(counters.pending, delta));
    }
}

void PublishPendingDeltas() {
    const auto gold = g_gold.pending.exchange(0, std::memory_order_acq_rel);
    const auto xp = g_xp.pending.exchange(0, std::memory_order_acq_rel);
    const auto kills = g_kills.pending.exchange(0, std::memory_order_acq_rel);
    if (gold <= 0 && xp <= 0 && kills <= 0) {
        return;
    }

    hsot::protocol::SessionDeltaEvent event{};
    event.envelope = MakeEnvelope();
    event.gold = gold;
    event.xp = xp;
    event.kills = kills;
    event.source.assign(kEventSource);
    if (PublishLine(hsot::protocol::SerializeNdjson(event)) != hsot::PublishResult::queued) {
        RequeueDelta(g_gold, gold);
        RequeueDelta(g_xp, xp);
        RequeueDelta(g_kills, kills);
    }
}

std::uint64_t g_magic_find_published_ms{};

void LogMagicFindProbe() {
    const int calls = g_mf_probe.calls.load(std::memory_order_relaxed);
    static int last_calls = 0;
    if (calls == last_calls || g_mf_probe_logged.load(std::memory_order_relaxed) >= 12) {
        return;
    }
    last_calls = calls;
    g_mf_probe_logged.fetch_add(1, std::memory_order_relaxed);
    const int count = g_mf_probe.count.load(std::memory_order_relaxed);
    std::string line = "mf probe calls=" + std::to_string(calls) + " elements=[";
    for (int i = 0; i < count && i < 4; ++i) {
        if (i) line += ",";
        line += std::to_string(std::bit_cast<double>(g_mf_probe.element[i].load(std::memory_order_relaxed)));
    }
    line += "] a5=" + std::to_string(std::bit_cast<double>(g_mf_probe.arg5.load(std::memory_order_relaxed)));
    line += " a7=" + std::to_string(std::bit_cast<double>(g_mf_probe.arg7.load(std::memory_order_relaxed)));
    LogFile(line);
}

void PublishPendingVitals() {
    LogMagicFindProbe();
    if (!g_magic_find_dirty.load(std::memory_order_acquire)) {
        return;
    }
    // Magic find is recomputed by the game many times a second; one line a
    // second is all the dashboard can show anyway.
    const auto now = MonotonicMilliseconds();
    if (g_magic_find_published_ms != 0U && now - g_magic_find_published_ms < 1000U) {
        return;
    }
    hsot::protocol::VitalsEvent event{};
    event.envelope = MakeEnvelope();
    event.magic_find = std::bit_cast<double>(g_magic_find_bits.load(std::memory_order_acquire));
    event.source.assign(kEventSource);
    if (PublishLine(hsot::protocol::SerializeNdjson(event)) == hsot::PublishResult::queued) {
        g_magic_find_dirty.store(false, std::memory_order_release);
        g_magic_find_published_ms = now;
    }
}

void PublishPendingZone() {
    if (!g_zone_dirty.load(std::memory_order_acquire)) {
        return;
    }
    ZoneSnapshot snapshot{};
    // The hook side only ever try-locks; the worker can afford a short spin.
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!g_zone_guard.test_and_set(std::memory_order_acquire)) {
            snapshot = g_zone_slot;
            g_zone_guard.clear(std::memory_order_release);
            g_zone_dirty.store(false, std::memory_order_release);
            break;
        }
        if (attempt == 63) {
            return;
        }
        std::this_thread::yield();
    }

    hsot::protocol::RoomEvent room{};
    room.envelope = MakeEnvelope();
    room.room.assign(snapshot.room);
    bool ok = PublishLine(hsot::protocol::SerializeNdjson(room)) == hsot::PublishResult::queued;

    if (snapshot.zone_known) {
        hsot::protocol::SatanicZoneEvent zone{};
        zone.envelope = MakeEnvelope();
        zone.zone.assign(snapshot.zone);
        zone.buffs.assign(snapshot.buffs, snapshot.buffs + snapshot.buff_count);
        zone.debuffs.assign(snapshot.debuffs, snapshot.debuffs + snapshot.debuff_count);
        ok = PublishLine(hsot::protocol::SerializeNdjson(zone)) == hsot::PublishResult::queued && ok;
    }
    if (snapshot.satanic_here >= 0) {
        hsot::protocol::VitalsEvent vitals{};
        vitals.envelope = MakeEnvelope();
        vitals.satanic_here = snapshot.satanic_here == 1;
        if (g_magic_find_seen.load(std::memory_order_acquire)) {
            vitals.magic_find = std::bit_cast<double>(g_magic_find_bits.load(std::memory_order_acquire));
        } else {
            vitals.omit_magic_find = true;
        }
        vitals.source.assign(kEventSource);
        ok = PublishLine(hsot::protocol::SerializeNdjson(vitals)) == hsot::PublishResult::queued && ok;
    }
    if (!ok) {
        // The transport was full; try the same snapshot again next cycle.
        g_zone_dirty.store(true, std::memory_order_release);
    }
}

[[nodiscard]] std::string_view RarityName(const std::int64_t rarity) noexcept {
    switch (rarity) {
    case 4: return "Set";
    case 6: return "Satanic";
    case 7: return "Angelic";
    case 9: return "Heroic";
    case 10: return "Unholy";
    default: return {};
    }
}

[[nodiscard]] bool PublishDrop(const DropSnapshot& snapshot) {
    hsot::protocol::GroundDropEvent event{};
    event.envelope = MakeEnvelope();
    event.event_id = "s10-" + std::to_string(event.envelope.process_id) + "-" +
        std::to_string(event.envelope.sequence);
    event.source = "game_ground_item_create";
    event.rarity_id = static_cast<std::int32_t>(snapshot.rarity);
    const auto rarity_name = RarityName(snapshot.rarity);
    if (!rarity_name.empty()) {
        event.rarity_name = std::string(rarity_name);
    }
    event.item_type = snapshot.item_type;
    event.item_id = snapshot.item_id;
    if (snapshot.weapon_type > 0) {
        event.weapon_type = snapshot.weapon_type;
    }
    if (snapshot.name[0] != '\0') {
        event.item_name = std::string(snapshot.name);
    }
    event.amount = 1;
    if (PublishLine(hsot::protocol::SerializeNdjson(event)) != hsot::PublishResult::queued) {
        return false;
    }
    return true;
}

[[nodiscard]] std::size_t PublishPendingDrops() {
    std::size_t published{};
    if (g_retry_drop) {
        if (!PublishDrop(*g_retry_drop)) {
            return published;
        }
        g_retry_drop.reset();
        ++published;
    }

    DropSnapshot snapshot{};
    while (published < kMaximumDropsPerPublishCycle && g_drop_snapshots.TryPop(snapshot)) {
        if (!PublishDrop(snapshot)) {
            g_retry_drop = snapshot;
            return published;
        }
        ++published;
    }
    return published;
}

void PublishWorkerLoop() noexcept {
    static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
    std::unique_lock wait_lock(g_publish_wait_mutex);
    while (!g_stopping.load(std::memory_order_acquire)) {
        // Fixed-rate polling keeps every wakeup and all serialization off the
        // game thread while limiting live-counter latency to one render frame.
        g_publish_wake.wait_for(wait_lock, std::chrono::milliseconds(16));
        if (g_stopping.load(std::memory_order_acquire)) {
            break;
        }

        wait_lock.unlock();
        try {
            PublishPendingDeltas();
            static_cast<void>(PublishPendingDrops());
            PublishPendingZone();
            PublishPendingVitals();
        } catch (...) {
            // Tracking is fail-open: malformed transient state must never
            // terminate the game or the producer's background publisher.
        }
        wait_lock.lock();
    }
}

[[nodiscard]] bool StartPublishWorker(std::string& detail) noexcept {
    try {
        g_publish_worker = std::thread(&PublishWorkerLoop);
        return true;
    } catch (...) {
        detail = "background publisher thread could not start";
        return false;
    }
}

void StopPublishWorker() noexcept {
    g_stopping.store(true, std::memory_order_release);
    g_publish_wake.notify_all();
    if (!g_publish_worker.joinable()) {
        return;
    }
    try {
        g_publish_worker.join();
    } catch (...) {
        // Aurie is already unloading. Avoid allowing cleanup exceptions to
        // cross the module boundary.
    }
}

void StopTransport() noexcept {
    g_stopping.store(true, std::memory_order_release);
    if (g_transport_stopped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::scoped_lock lock(g_transport_mutex);
    if (g_transport) {
        g_transport->Stop();
    }
}

} // namespace

EXPORTED void ModuleOperationCallback(
    IN AurieModule* affected_module,
    IN AurieModuleOperationType operation_type,
    OPTIONAL IN OUT AurieOperationInfo* operation_info) {
    if (affected_module != g_ArSelfModule || operation_type != AURIE_OPERATION_UNLOAD) {
        return;
    }
    if (!operation_info || !operation_info->IsFutureCall) {
        return;
    }
    g_stopping.store(true, std::memory_order_release);
    StopPublishWorker();
    const auto dropped = g_drop_queue_dropped.exchange(0U, std::memory_order_acq_rel);
    if (dropped != 0U && g_yytk) {
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] %llu rare drops were skipped to keep gameplay non-blocking",
            static_cast<unsigned long long>(dropped));
    }
    StopTransport();
}

EXPORTED AurieStatus ModuleInitialize(
    IN AurieModule* module,
    IN const fs::path& module_path) {
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(module_path);

    g_stopping.store(false, std::memory_order_release);
    g_transport_stopped.store(false, std::memory_order_release);
    g_executable_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    g_yytk = YYTK::GetInterface();
    if (!g_yytk) {
        return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;
    }

    LogFile("module loaded; starting local transport");
    g_transport = hsot::CreateLocalNamedPipeTransport(512U);
    std::string transport_detail;
    if (!g_transport || !g_transport->Start(GetCurrentProcessId(), transport_detail)) {
        LogFile("transport failed: " + transport_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] local transport failed: %s", transport_detail.c_str());
        return AURIE_SUCCESS;
    }

    std::string gate_detail;
    bool exact_profile = false;
    if (!SelectProfile(gate_detail, exact_profile)) {
        PublishStatus(hsot::protocol::BridgeState::blocked, "unsupported_build", gate_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", gate_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    std::string hook_detail;
    InstallReport report{};
    if (!InstallNamedHooks(hook_detail, report)) {
        PublishStatus(hsot::protocol::BridgeState::blocked, "named_route_unavailable", hook_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", hook_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    std::string publisher_detail;
    if (!StartPublishWorker(publisher_detail)) {
        g_stopping.store(true, std::memory_order_release);
        RemoveInstalledHooks();
        PublishStatus(
            hsot::protocol::BridgeState::blocked,
            "publisher_start_failed",
            publisher_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", publisher_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    const std::string status_detail = gate_detail + " | " + hook_detail;
    LogFile("pipe " + g_transport->PipeNameUtf8());
    PublishStatus(
        hsot::protocol::BridgeState::transport_ready,
        exact_profile ? "exact_build_ready" : "adaptive_build_ready",
        status_detail);
    g_yytk->Print(CM_LIGHTGREEN,
        "[HS Offline Tracker] %s producer ready: %s",
        exact_profile ? "exact" : "adaptive", hook_detail.c_str());
    return AURIE_SUCCESS;
}

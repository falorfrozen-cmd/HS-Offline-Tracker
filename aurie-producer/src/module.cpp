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
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

struct DropSnapshot {
    std::int64_t rarity{};
    std::int64_t item_type{};
    std::int64_t item_id{};
};

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

[[nodiscard]] std::uint64_t UnixMilliseconds() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
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

void PublishStatus(
    const hsot::protocol::BridgeState state,
    const std::string_view code,
    const std::string_view detail) {
    hsot::protocol::StatusEvent event{};
    event.envelope = MakeEnvelope();
    event.state = state;
    event.code.assign(code);
    event.detail.assign(detail);
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

[[nodiscard]] bool ExactBuildAllowed(std::string& detail) noexcept {
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
    if (!g_active_profile) {
        detail = "unsupported exact code build: machine=" +
            std::to_string(actual.fingerprint.machine) + " timestamp=" +
            std::to_string(actual.fingerprint.pe_timestamp) + " text_size=" +
            std::to_string(actual.fingerprint.text_raw_size) + " text_sha256=" +
            actual.fingerprint.text_sha256_hex + " full_size=" +
            std::to_string(actual.fingerprint.file_size) + " full_sha256=" +
            actual.fingerprint.sha256_hex;
        return false;
    }

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

[[nodiscard]] std::optional<std::int64_t> CaptureDelta(
    const int argument_count,
    RValue** arguments,
    const int expected_argument_count,
    const int value_argument_index) noexcept {
    if (argument_count != expected_argument_count || value_argument_index < 0 ||
        value_argument_index >= argument_count || !arguments ||
        !arguments[value_argument_index]) {
        return std::nullopt;
    }

    const RValue& value = *arguments[value_argument_index];
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
    return hsot::aurie::DecodePositiveIntegralDelta(observed);
}

[[nodiscard]] std::optional<std::int64_t> CaptureKillStatisticId(
    const int argument_count,
    RValue** arguments) noexcept {
    if (argument_count != 1 || !arguments || !arguments[0]) {
        return std::nullopt;
    }

    const RValue& value = *arguments[0];
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
    return hsot::aurie::DecodeNonNegativeIntegralId(observed);
}

[[nodiscard]] std::optional<std::int64_t> CaptureNonNegativeValue(
    const RValue& value) noexcept {
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
    return hsot::aurie::DecodeNonNegativeIntegralId(observed);
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
        // The exact-build caller allowlist and original-first hook prove that
        // this is the newly-created live Loot_Ground instance. CallBuiltin()
        // would execute @@GlobalScope@@ first and allocate another argument
        // vector; CallBuiltinEx() performs only the one getter we need.
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
        // builtin only as a compatibility fallback; the supported S10 build
        // takes this fast path for every item field.
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
    // numeric-string keys such as "27". GetInstanceMember's fallback invokes
    // variable_instance_exists and misclassifies these structs as instances;
    // variable_struct_get is the native and already live-proven access path.
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
    return DropSnapshot{*rarity, *item_type, *item_id};
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
    const bool from_reviewed_ground_call = g_active_profile &&
        hsot::aurie::IsGroundItemCreateCallerRva(
            g_active_profile->drop_route, caller_rva);

    if (!g_original_ground_init) {
        return result;
    }

    // Original-first is deliberate. LootGroundInit is allowed to complete every
    // native placement side effect before we read the engine-owned item struct.
    RValue& original_result =
        g_original_ground_init(self, other, result, argument_count, arguments);
    if (!from_reviewed_ground_call) {
        return original_result;
    }

    if (g_stopping.load(std::memory_order_acquire)) {
        return original_result;
    }

    std::optional<DropSnapshot> snapshot;
    if (argument_count == 2 && arguments && arguments[0]) {
        // Static data flow proves arg0 is the newly created Loot_Ground
        // instance. Its finalized item is ground.itemInstance; arg1 is a
        // caller option and must never be decoded as an item.
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

struct HookBinding {
    std::string_view routine_name;
    std::string_view hook_id;
    PVOID detour{};
    PFUNC_YYGMLScript* original{};
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
    return true;
}

[[nodiscard]] bool RemoveInstalledHooks(const std::size_t installed_count) noexcept {
    if (!g_active_profile) {
        return true;
    }
    const auto& routes = g_active_profile->routes;
    struct InstalledHook {
        std::string_view hook_id;
        PFUNC_YYGMLScript* original;
    };
    std::array<InstalledHook, 4U> hooks{};
    std::size_t hook_count{};
    hooks[hook_count++] = InstalledHook{routes[0].hook_id, &g_original_gold};
    hooks[hook_count++] = InstalledHook{routes[1].hook_id, &g_original_xp};
    hooks[hook_count++] = InstalledHook{routes[2].hook_id, &g_original_kill_candidate};
    if (g_active_profile->drop_route.enabled) {
        hooks[hook_count++] = InstalledHook{
            g_active_profile->drop_route.hook_id, &g_original_ground_init};
    }
    bool all_removed = true;
    const auto remove_count = installed_count < hook_count ? installed_count : hook_count;
    for (std::size_t index = remove_count; index > 0U; --index) {
        const auto& hook = hooks[index - 1U];
        const std::string hook_id(hook.hook_id);
        const auto status = MmRemoveHook(g_ArSelfModule, hook_id.c_str());
        if (AurieSuccess(status)) {
            *hook.original = nullptr;
        } else {
            all_removed = false;
            if (g_yytk) {
                g_yytk->Print(CM_LIGHTRED,
                    "[HS Offline Tracker] hook rollback failed for %s status=%d; keeping passthrough trampoline",
                    hook_id.c_str(), static_cast<int>(status));
            }
        }
    }
    return all_removed;
}

[[nodiscard]] bool InstallNamedHooks(std::string& detail) noexcept {
    if (!g_active_profile) {
        detail = "no exact producer profile is active";
        return false;
    }
    const auto& routes = g_active_profile->routes;
    const auto& drop = g_active_profile->drop_route;
    std::array<HookBinding, 4U> bindings{};
    std::size_t binding_count{};
    bindings[binding_count++] = HookBinding{routes[0].routine_name, routes[0].hook_id,
        reinterpret_cast<PVOID>(&HookGold), &g_original_gold};
    bindings[binding_count++] = HookBinding{routes[1].routine_name, routes[1].hook_id,
        reinterpret_cast<PVOID>(&HookXp), &g_original_xp};
    bindings[binding_count++] = HookBinding{routes[2].routine_name, routes[2].hook_id,
        reinterpret_cast<PVOID>(&HookKillCandidate), &g_original_kill_candidate};
    if (drop.enabled) {
        bindings[binding_count++] = HookBinding{drop.routine_name, drop.hook_id,
            reinterpret_cast<PVOID>(&HookGroundInit), &g_original_ground_init};
    }

    std::size_t installed{};
    for (std::size_t index = 0U; index < binding_count; ++index) {
        if (!ResolveAndInstallHook(bindings[index], detail)) {
            g_stopping.store(true, std::memory_order_release);
            static_cast<void>(RemoveInstalledHooks(installed));
            return false;
        }
        ++installed;
    }
    detail = "Gold, XP and validated kill routes enabled";
    detail += drop.enabled
        ? "; scoped ground-item route enabled"
        : "; rare drops unavailable for this profile";
    return true;
}

[[nodiscard]] std::size_t ExpectedHookCount() noexcept {
    if (!g_active_profile) {
        return 0U;
    }
    return g_active_profile->routes.size() +
        (g_active_profile->drop_route.enabled ? 1U : 0U);
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

    g_transport = hsot::CreateLocalNamedPipeTransport(512U);
    std::string transport_detail;
    if (!g_transport || !g_transport->Start(GetCurrentProcessId(), transport_detail)) {
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] local transport failed: %s", transport_detail.c_str());
        return AURIE_SUCCESS;
    }

    std::string gate_detail;
    if (!ExactBuildAllowed(gate_detail)) {
        PublishStatus(hsot::protocol::BridgeState::blocked, "unsupported_build", gate_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", gate_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    std::string hook_detail;
    if (!InstallNamedHooks(hook_detail)) {
        PublishStatus(hsot::protocol::BridgeState::blocked, "named_route_unavailable", hook_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", hook_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    std::string publisher_detail;
    if (!StartPublishWorker(publisher_detail)) {
        g_stopping.store(true, std::memory_order_release);
        static_cast<void>(RemoveInstalledHooks(ExpectedHookCount()));
        PublishStatus(
            hsot::protocol::BridgeState::blocked,
            "publisher_start_failed",
            publisher_detail);
        g_yytk->Print(CM_LIGHTRED,
            "[HS Offline Tracker] producer blocked: %s", publisher_detail.c_str());
        StopTransport();
        return AURIE_SUCCESS;
    }

    const bool drops_enabled = g_active_profile && g_active_profile->drop_route.enabled;
    PublishStatus(
        hsot::protocol::BridgeState::transport_ready,
        drops_enabled ? "gold_xp_kills_drops_ready" : "gold_xp_kills_ready",
        drops_enabled
            ? "GoldLogAdd, ExperienceUpdate, validated EnemyAddStatistics and ground-item drops are enabled"
            : "GoldLogAdd, ExperienceUpdate and validated EnemyAddStatistics are enabled");
    g_yytk->Print(CM_LIGHTGREEN,
        drops_enabled
            ? "[HS Offline Tracker] named producer ready; gold, XP, kill and ground-item sensors enabled"
            : "[HS Offline Tracker] named producer ready; gold, XP and kill sensors enabled");
    return AURIE_SUCCESS;
}

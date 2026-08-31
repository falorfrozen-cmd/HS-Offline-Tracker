#include "hsot/bridge_export.h"

#include "hsot/event_protocol.h"
#include "hsot/event_transport.h"
#include "hsot/gamemaker_readonly.h"
#include "sensor_validation.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr std::size_t kSnapshotCapacity = 512U;
constexpr std::uint64_t kWindowsToUnixEpochMilliseconds = 11644473600000ULL;

struct DropSnapshot {
    std::uint64_t sequence{};
    std::uint64_t unix_time_ms{};
    std::int64_t rarity{};
    std::int64_t item_type{};
    std::int64_t item_id{};
};

class SnapshotQueue final {
public:
    [[nodiscard]] bool TryPush(const DropSnapshot& snapshot) noexcept {
        if (producer_guard_.test_and_set(std::memory_order_acquire)) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto read = read_index_.load(std::memory_order_acquire);
        if (write - read >= kSnapshotCapacity) {
            producer_guard_.clear(std::memory_order_release);
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        entries_[write % kSnapshotCapacity] = snapshot;
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
        snapshot = entries_[read % kSnapshotCapacity];
        read_index_.store(read + 1U, std::memory_order_release);
        return true;
    }

    void Reset() noexcept {
        read_index_.store(0U, std::memory_order_relaxed);
        write_index_.store(0U, std::memory_order_relaxed);
        dropped_.store(0U, std::memory_order_relaxed);
        producer_guard_.clear(std::memory_order_release);
    }

private:
    std::array<DropSnapshot, kSnapshotCapacity> entries_{};
    std::atomic_size_t read_index_{};
    std::atomic_size_t write_index_{};
    std::atomic_uint64_t dropped_{};
    std::atomic_flag producer_guard_ = ATOMIC_FLAG_INIT;
};

struct SensorState {
    std::mutex lifecycle_mutex;
    std::atomic_bool accepting_events{};
    std::atomic_bool running{};
    std::atomic_bool encoder_stop{};
    std::atomic_uint32_t active_callbacks{};
    std::atomic_uint64_t next_sequence{1U};
    SnapshotQueue snapshots;
    std::unique_ptr<hsot::IEventTransport> transport;
    std::thread encoder_thread;
    hsot::gm::ScriptFunction original{};
    void* hook_target{};
    bool minhook_initialized{};
    bool hook_created{};
    bool hook_enabled{};
    std::uint32_t process_id{};
    std::string build_id;
    std::size_t rarity_argument_index{};
    std::size_t item_type_argument_index{};
    std::size_t item_id_argument_index{};
};

SensorState g_sensor;

class CallbackGuard final {
public:
    CallbackGuard() noexcept {
        g_sensor.active_callbacks.fetch_add(1U, std::memory_order_acq_rel);
    }

    ~CallbackGuard() {
        g_sensor.active_callbacks.fetch_sub(1U, std::memory_order_acq_rel);
    }

    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};

bool IsReadableProtection(const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
        return false;
    }
    const auto basic = protection & 0xFFU;
    return basic == PAGE_READONLY || basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
        basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE || basic == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadableMemory(const void* address, const std::size_t bytes) noexcept {
    if (address == nullptr || bytes == 0U) {
        return false;
    }
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    if (cursor > (std::numeric_limits<std::uintptr_t>::max)() - bytes) {
        return false;
    }
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0U ||
            memory.State != MEM_COMMIT || !IsReadableProtection(memory.Protect)) {
            return false;
        }
        const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (region_begin > (std::numeric_limits<std::uintptr_t>::max)() - memory.RegionSize) {
            return false;
        }
        const auto region_end = region_begin + memory.RegionSize;
        if (region_end <= cursor) {
            return false;
        }
        cursor = (std::min)(region_end, end);
    }
    return true;
}

bool SafeCopyMemory(void* destination, const void* source, const std::size_t bytes) noexcept {
    if (!IsReadableMemory(source, bytes)) {
        return false;
    }
    __try {
        std::memcpy(destination, source, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t ValueKind(const hsot::gm::Value& value) noexcept {
    return value.kind & 0x1FU;
}

bool ValueToInteger(const hsot::gm::Value& value, std::int64_t& result) noexcept {
    if (value.flags != 0U) {
        return false;
    }
    switch (static_cast<hsot::gm::ValueKind>(ValueKind(value))) {
    case hsot::gm::ValueKind::int32:
        result = value.payload.int32_value;
        return true;
    case hsot::gm::ValueKind::boolean:
        if (!std::isfinite(value.payload.real_value)) {
            return false;
        }
        result = value.payload.real_value == 0.0 ? 0 : 1;
        return true;
    case hsot::gm::ValueKind::int64:
        result = value.payload.int64_value;
        return true;
    case hsot::gm::ValueKind::real: {
        const auto number = value.payload.real_value;
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < static_cast<double>((std::numeric_limits<std::int64_t>::min)()) ||
            number > static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }
        result = static_cast<std::int64_t>(number);
        return true;
    }
    default:
        return false;
    }
}

bool ValueIsTrue(const hsot::gm::Value& value) noexcept {
    if (value.flags != 0U) {
        return false;
    }
    switch (static_cast<hsot::gm::ValueKind>(ValueKind(value))) {
    case hsot::gm::ValueKind::int32:
        return value.payload.int32_value != 0;
    case hsot::gm::ValueKind::boolean:
        return std::isfinite(value.payload.real_value) && value.payload.real_value != 0.0;
    case hsot::gm::ValueKind::int64:
        return value.payload.int64_value != 0;
    case hsot::gm::ValueKind::real:
        return std::isfinite(value.payload.real_value) && value.payload.real_value != 0.0;
    default:
        return false;
    }
}

std::uint64_t UnixTimeMilliseconds() noexcept {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    const auto milliseconds = ticks.QuadPart / 10000ULL;
    return milliseconds >= kWindowsToUnixEpochMilliseconds
        ? milliseconds - kWindowsToUnixEpochMilliseconds
        : 0U;
}

bool TryReadSnapshot(
    const int argument_count,
    hsot::gm::Value** arguments,
    DropSnapshot& snapshot) noexcept {
    if (argument_count != 3 || arguments == nullptr) {
        return false;
    }

    std::array<hsot::gm::Value*, 3U> pointers{};
    if (!SafeCopyMemory(pointers.data(), arguments, sizeof(pointers))) {
        return false;
    }
    std::array<hsot::gm::Value, 3U> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (pointers[index] == nullptr || !SafeCopyMemory(&values[index], pointers[index], sizeof(values[index]))) {
            return false;
        }
    }

    if (g_sensor.rarity_argument_index >= values.size() ||
        g_sensor.item_type_argument_index >= values.size() ||
        g_sensor.item_id_argument_index >= values.size() ||
        !ValueToInteger(values[g_sensor.rarity_argument_index], snapshot.rarity) ||
        !ValueToInteger(values[g_sensor.item_type_argument_index], snapshot.item_type) ||
        !ValueToInteger(values[g_sensor.item_id_argument_index], snapshot.item_id)) {
        return false;
    }

    // These strict bounds match the tracker's reviewed identity resolver. Values outside
    // them indicate a changed ABI or a different call contract, so no event is emitted.
    return snapshot.rarity >= 0 && snapshot.rarity <= 32 &&
        snapshot.item_type >= 0 && snapshot.item_type <= 255 &&
        snapshot.item_id >= 0 && snapshot.item_id <= 65535;
}

hsot::gm::Value& RareAnnouncementHook(
    hsot::gm::Instance* self,
    hsot::gm::Instance* other,
    hsot::gm::Value& result,
    const int argument_count,
    hsot::gm::Value** arguments) {
    CallbackGuard callback_guard;
    // Original-first is deliberate: this sensor never changes arguments, the return
    // value, drop generation, announcement policy, or any other game state.
    hsot::gm::Value& returned = g_sensor.original(self, other, result, argument_count, arguments);
    if (!g_sensor.accepting_events.load(std::memory_order_acquire) || !ValueIsTrue(returned)) {
        return returned;
    }

    DropSnapshot snapshot{};
    if (!TryReadSnapshot(argument_count, arguments, snapshot)) {
        return returned;
    }
    snapshot.sequence = g_sensor.next_sequence.fetch_add(1U, std::memory_order_relaxed);
    snapshot.unix_time_ms = UnixTimeMilliseconds();
    static_cast<void>(g_sensor.snapshots.TryPush(snapshot));
    return returned;
}

void EncoderLoop() noexcept {
    try {
        for (;;) {
            DropSnapshot snapshot{};
            if (!g_sensor.snapshots.TryPop(snapshot)) {
                if (g_sensor.encoder_stop.load(std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            hsot::protocol::GroundDropEvent event{};
            event.envelope.sequence = snapshot.sequence;
            event.envelope.emitted_at_unix_ms = snapshot.unix_time_ms;
            event.envelope.process_id = g_sensor.process_id;
            event.envelope.build_id = g_sensor.build_id;
            event.event_id = "s10-" + std::to_string(g_sensor.process_id) + "-" + std::to_string(snapshot.sequence);
            event.source = "game_rare_announcement";
            event.rarity_id = static_cast<std::int32_t>(snapshot.rarity);
            event.item_type = snapshot.item_type;
            event.item_id = snapshot.item_id;
            event.amount = 1;

            const auto ndjson = hsot::protocol::SerializeNdjson(event);
            if (g_sensor.transport) {
                static_cast<void>(g_sensor.transport->TryPublish(ndjson));
            }
        }
    } catch (...) {
        // A formatting/allocation failure only stops capture output. It must never
        // propagate through the game thread or cause a fabricated event.
        g_sensor.accepting_events.store(false, std::memory_order_release);
    }
}

std::string MinHookFailure(const char* operation, const MH_STATUS status) {
    std::string detail(operation);
    detail += " failed: ";
    detail += MH_StatusToString(status);
    return detail;
}

void StopLocked() noexcept {
    g_sensor.accepting_events.store(false, std::memory_order_release);
    g_sensor.running.store(false, std::memory_order_release);

    if (g_sensor.hook_enabled && g_sensor.hook_target != nullptr) {
        static_cast<void>(MH_DisableHook(g_sensor.hook_target));
        g_sensor.hook_enabled = false;
    }
    while (g_sensor.active_callbacks.load(std::memory_order_acquire) != 0U) {
        Sleep(1U);
    }
    if (g_sensor.hook_created && g_sensor.hook_target != nullptr) {
        static_cast<void>(MH_RemoveHook(g_sensor.hook_target));
        g_sensor.hook_created = false;
    }

    g_sensor.encoder_stop.store(true, std::memory_order_release);
    if (g_sensor.encoder_thread.joinable()) {
        g_sensor.encoder_thread.join();
    }
    if (g_sensor.transport) {
        g_sensor.transport->Stop();
        g_sensor.transport.reset();
    }
    if (g_sensor.minhook_initialized) {
        static_cast<void>(MH_Uninitialize());
        g_sensor.minhook_initialized = false;
    }

    g_sensor.original = nullptr;
    g_sensor.hook_target = nullptr;
    g_sensor.build_id.clear();
    g_sensor.snapshots.Reset();
}

} // namespace

hsot::BridgeValidationCode HSOT_BridgeStartSensor(
    char* detail,
    const std::size_t detail_capacity) noexcept {
    std::scoped_lock lock(g_sensor.lifecycle_mutex);
    if (g_sensor.running.load(std::memory_order_acquire)) {
        hsot::CopyBridgeDetail("sensor is already running", detail, detail_capacity);
        return hsot::BridgeValidationCode::already_started;
    }

    try {
        const auto validation = hsot::ValidateCurrentHostForSensor();
        if (validation.code != hsot::BridgeValidationCode::validated || validation.profile == nullptr) {
            hsot::CopyBridgeDetail(validation.detail, detail, detail_capacity);
            return validation.code;
        }

        g_sensor.snapshots.Reset();
        g_sensor.active_callbacks.store(0U, std::memory_order_relaxed);
        g_sensor.encoder_stop.store(false, std::memory_order_release);
        g_sensor.next_sequence.store(1U, std::memory_order_relaxed);
        g_sensor.process_id = GetCurrentProcessId();
        g_sensor.build_id = validation.profile->build_id;
        g_sensor.rarity_argument_index = validation.profile->rarity_argument_index;
        g_sensor.item_type_argument_index = validation.profile->item_type_argument_index;
        g_sensor.item_id_argument_index = validation.profile->item_id_argument_index;
        g_sensor.hook_target = reinterpret_cast<void*>(validation.route.rare_announcement);

        g_sensor.transport = hsot::CreateLocalNamedPipeTransport();
        std::string transport_detail;
        if (!g_sensor.transport || !g_sensor.transport->Start(g_sensor.process_id, transport_detail)) {
            StopLocked();
            hsot::CopyBridgeDetail(
                transport_detail.empty() ? "failed to create local transport" : transport_detail,
                detail,
                detail_capacity);
            return hsot::BridgeValidationCode::transport_start_failed;
        }

        try {
            g_sensor.encoder_thread = std::thread(EncoderLoop);
        } catch (...) {
            StopLocked();
            hsot::CopyBridgeDetail("failed to start event encoder thread", detail, detail_capacity);
            return hsot::BridgeValidationCode::transport_start_failed;
        }

        auto status = MH_Initialize();
        if (status != MH_OK) {
            const auto message = MinHookFailure("MH_Initialize", status);
            StopLocked();
            hsot::CopyBridgeDetail(message, detail, detail_capacity);
            return hsot::BridgeValidationCode::hook_start_failed;
        }
        g_sensor.minhook_initialized = true;

        hsot::gm::ScriptFunction detour = &RareAnnouncementHook;
        void* detour_address{};
        static_assert(sizeof(detour_address) == sizeof(detour));
        std::memcpy(&detour_address, &detour, sizeof(detour_address));
        void* original_address{};
        status = MH_CreateHook(g_sensor.hook_target, detour_address, &original_address);
        if (status != MH_OK || original_address == nullptr) {
            const auto message = status == MH_OK
                ? std::string("MH_CreateHook returned a null original trampoline")
                : MinHookFailure("MH_CreateHook", status);
            StopLocked();
            hsot::CopyBridgeDetail(message, detail, detail_capacity);
            return hsot::BridgeValidationCode::hook_start_failed;
        }
        static_assert(sizeof(original_address) == sizeof(g_sensor.original));
        std::memcpy(&g_sensor.original, &original_address, sizeof(g_sensor.original));
        g_sensor.hook_created = true;

        status = MH_EnableHook(g_sensor.hook_target);
        if (status != MH_OK) {
            const auto message = MinHookFailure("MH_EnableHook", status);
            StopLocked();
            hsot::CopyBridgeDetail(message, detail, detail_capacity);
            return hsot::BridgeValidationCode::hook_start_failed;
        }
        g_sensor.hook_enabled = true;
        g_sensor.accepting_events.store(true, std::memory_order_release);
        g_sensor.running.store(true, std::memory_order_release);

        hsot::protocol::StatusEvent event{};
        event.envelope.sequence = g_sensor.next_sequence.fetch_add(1U, std::memory_order_relaxed);
        event.envelope.emitted_at_unix_ms = UnixTimeMilliseconds();
        event.envelope.process_id = g_sensor.process_id;
        event.envelope.build_id = g_sensor.build_id;
        event.state = hsot::protocol::BridgeState::transport_ready;
        event.code = "sensor_started";
        event.detail = "read-only rare ground-drop sensor active";
        static_cast<void>(g_sensor.transport->TryPublish(hsot::protocol::SerializeNdjson(event)));

        hsot::CopyBridgeDetail(
            "read-only sensor started; pipe " + g_sensor.transport->PipeNameUtf8(), detail, detail_capacity);
        return hsot::BridgeValidationCode::validated;
    } catch (...) {
        StopLocked();
        hsot::CopyBridgeDetail("unexpected sensor-start exception", detail, detail_capacity);
        return hsot::BridgeValidationCode::internal_error;
    }
}

void HSOT_BridgeStopSensor() noexcept {
    std::scoped_lock lock(g_sensor.lifecycle_mutex);
    StopLocked();
}

bool HSOT_BridgeSensorRunning() noexcept {
    return g_sensor.running.load(std::memory_order_acquire);
}

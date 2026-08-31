#pragma once

#include <cstdint>

namespace hsot::gm {

struct Instance;

enum class ValueKind : std::uint32_t {
    real = 0,
    string = 1,
    array = 2,
    pointer = 3,
    vec3 = 4,
    undefined = 5,
    object = 6,
    int32 = 7,
    vec4 = 8,
    matrix = 9,
    int64 = 10,
    accessor = 11,
    null_value = 12,
    boolean = 13,
    iterator = 14,
    reference = 15,
};

#pragma pack(push, 4)
struct Value {
    union Payload {
        std::int32_t int32_value;
        std::int64_t int64_value;
        double real_value;
        void* pointer_value;

        constexpr Payload() noexcept : int64_value(0) {}
    } payload;
    std::uint32_t flags{};
    std::uint32_t kind{static_cast<std::uint32_t>(ValueKind::undefined)};
};
#pragma pack(pop)

static_assert(sizeof(Value) == 16U, "reviewed Season 10 value ABI must remain 16 bytes");

using ScriptFunction = Value& (*)(
    Instance* self,
    Instance* other,
    Value& result,
    int argument_count,
    Value** arguments);

} // namespace hsot::gm


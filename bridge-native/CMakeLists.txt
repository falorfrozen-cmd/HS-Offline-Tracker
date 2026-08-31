cmake_minimum_required(VERSION 3.24)

project(HSOfflineTrackerBridge VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "HSOfflineTrackerBridge currently targets Windows only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "HSOfflineTrackerBridge currently requires the Visual C++ toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HSOfflineTrackerBridge must be built for x64.")
endif()

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(minhook)

add_library(HSOfflineTrackerBridge SHARED
    src/bridge_runtime.cpp
    src/build_fingerprint_win32.cpp
    src/dllmain.cpp
    src/event_protocol.cpp
    src/named_pipe_transport_win32.cpp
    src/named_routine_resolver_win32.cpp
    src/route_validation.cpp
    src/sensor_runtime.cpp
    src/sensor_validation.cpp
)

target_include_directories(HSOfflineTrackerBridge
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(HSOfflineTrackerBridge PUBLIC cxx_std_20)
target_compile_definitions(HSOfflineTrackerBridge
    PRIVATE
        HSOT_BRIDGE_BUILD
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
)
target_compile_options(HSOfflineTrackerBridge PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(HSOfflineTrackerBridge PRIVATE bcrypt minhook)

include(CTest)
if(BUILD_TESTING)
    add_executable(hsot_protocol_smoke
        tests/protocol_smoke.cpp
        src/event_protocol.cpp
    )
    target_include_directories(hsot_protocol_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(hsot_protocol_smoke PRIVATE cxx_std_20)
    target_compile_definitions(hsot_protocol_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(hsot_protocol_smoke PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hsot_protocol_smoke COMMAND hsot_protocol_smoke)

    add_executable(hsot_build_gate_smoke
        tests/build_gate_smoke.cpp
        src/build_fingerprint_win32.cpp
    )
    target_include_directories(hsot_build_gate_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(hsot_build_gate_smoke PRIVATE cxx_std_20)
    target_compile_definitions(hsot_build_gate_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(hsot_build_gate_smoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(hsot_build_gate_smoke PRIVATE bcrypt)
    add_test(NAME hsot_build_gate_smoke COMMAND hsot_build_gate_smoke)
endif()

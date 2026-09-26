cmake_minimum_required(VERSION 3.24)

project(HSOfflineTrackerAurieProducer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "HSOfflineTrackerAurieProducer currently targets Windows only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "HSOfflineTrackerAurieProducer requires the Visual C++ toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HSOfflineTrackerAurieProducer must be built for x64.")
endif()

get_filename_component(
    _hsot_default_yytk_sdk
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ForgePact/plugin_build"
    ABSOLUTE)
set(YYTK_SDK_ROOT "${_hsot_default_yytk_sdk}" CACHE PATH
    "Directory containing include/YYToolkit and include/Aurie")
set(HSOT_BRIDGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../bridge-native")

foreach(_required_file IN ITEMS
    "${YYTK_SDK_ROOT}/include/YYToolkit/YYTK_Shared.hpp"
    "${YYTK_SDK_ROOT}/include/YYToolkit/YYTK_Shared_Types.cpp"
    "${YYTK_SDK_ROOT}/include/Aurie/shared.hpp"
    "${HSOT_BRIDGE_ROOT}/include/hsot/event_protocol.h")
    if(NOT EXISTS "${_required_file}")
        message(FATAL_ERROR "Required producer dependency is missing: ${_required_file}")
    endif()
endforeach()

add_library(hsot_counter_validation STATIC
    src/counter_validation.cpp)
target_include_directories(hsot_counter_validation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hsot_counter_validation PUBLIC cxx_std_20)
target_compile_options(hsot_counter_validation PRIVATE /W4 /permissive- /EHsc)

add_library(HSOfflineTrackerProducer MODULE
    src/module.cpp
    "${HSOT_BRIDGE_ROOT}/src/build_fingerprint_win32.cpp"
    "${HSOT_BRIDGE_ROOT}/src/event_protocol.cpp"
    "${HSOT_BRIDGE_ROOT}/src/named_pipe_transport_win32.cpp"
    "${YYTK_SDK_ROOT}/include/YYToolkit/YYTK_Shared_Types.cpp")
target_include_directories(HSOfflineTrackerProducer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${HSOT_BRIDGE_ROOT}/include"
    "${YYTK_SDK_ROOT}/include")
target_compile_features(HSOfflineTrackerProducer PRIVATE cxx_std_20)
target_compile_definitions(HSOfflineTrackerProducer PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE)
target_compile_options(HSOfflineTrackerProducer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(HSOfflineTrackerProducer PRIVATE
    hsot_counter_validation
    bcrypt
    user32)
set_target_properties(HSOfflineTrackerProducer PROPERTIES
    PREFIX ""
    OUTPUT_NAME "HSOfflineTrackerProducer"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(CTest)
if(BUILD_TESTING)
    add_executable(hsot_counter_validation_smoke
        tests/counter_validation_smoke.cpp)
    target_link_libraries(hsot_counter_validation_smoke PRIVATE hsot_counter_validation)
    target_compile_features(hsot_counter_validation_smoke PRIVATE cxx_std_20)
    target_compile_options(hsot_counter_validation_smoke PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hsot_counter_validation_smoke COMMAND hsot_counter_validation_smoke)

    add_executable(hsot_aurie_code_gate_smoke
        tests/code_gate_smoke.cpp
        "${HSOT_BRIDGE_ROOT}/src/build_fingerprint_win32.cpp")
    target_include_directories(hsot_aurie_code_gate_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${HSOT_BRIDGE_ROOT}/include")
    target_compile_features(hsot_aurie_code_gate_smoke PRIVATE cxx_std_20)
    target_compile_definitions(hsot_aurie_code_gate_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(hsot_aurie_code_gate_smoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(hsot_aurie_code_gate_smoke PRIVATE bcrypt)
    add_test(NAME hsot_aurie_code_gate_smoke COMMAND hsot_aurie_code_gate_smoke)

    add_executable(hsot_aurie_protocol_smoke
        "${HSOT_BRIDGE_ROOT}/tests/protocol_smoke.cpp"
        "${HSOT_BRIDGE_ROOT}/src/event_protocol.cpp")
    target_include_directories(hsot_aurie_protocol_smoke PRIVATE
        "${HSOT_BRIDGE_ROOT}/include")
    target_compile_features(hsot_aurie_protocol_smoke PRIVATE cxx_std_20)
    target_compile_definitions(hsot_aurie_protocol_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(hsot_aurie_protocol_smoke PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hsot_aurie_protocol_smoke COMMAND hsot_aurie_protocol_smoke)

    # How the producer's threads end at ExitProcess and at an Aurie unload,
    # including Aurie's own unload from DllMain. The probe runs the real pipe
    # transport and include/hsot_aurie/exit_safe_thread.h; the host stands in
    # for AurieCore.dll.
    add_library(hsot_exit_teardown_probe MODULE
        tests/exit_teardown_probe.cpp
        "${HSOT_BRIDGE_ROOT}/src/named_pipe_transport_win32.cpp")
    target_include_directories(hsot_exit_teardown_probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${HSOT_BRIDGE_ROOT}/include")
    add_library(hsot_exit_teardown_host MODULE
        tests/exit_teardown_host.cpp)
    add_executable(hsot_exit_teardown_smoke
        tests/exit_teardown_smoke.cpp)
    foreach(_exit_teardown_target IN ITEMS
        hsot_exit_teardown_probe hsot_exit_teardown_host hsot_exit_teardown_smoke)
        target_compile_features(${_exit_teardown_target} PRIVATE cxx_std_20)
        target_compile_definitions(${_exit_teardown_target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        target_compile_options(${_exit_teardown_target} PRIVATE /W4 /permissive- /EHsc)
        # One shared CRT, so the smoke's SIGABRT handler also catches an abort
        # in the probe instead of a fast fail reaching Windows Error Reporting.
        set_property(TARGET ${_exit_teardown_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endforeach()
    add_dependencies(hsot_exit_teardown_smoke hsot_exit_teardown_probe hsot_exit_teardown_host)
    add_test(NAME hsot_exit_teardown_smoke COMMAND hsot_exit_teardown_smoke
        $<TARGET_FILE:hsot_exit_teardown_probe> $<TARGET_FILE:hsot_exit_teardown_host>)
endif()

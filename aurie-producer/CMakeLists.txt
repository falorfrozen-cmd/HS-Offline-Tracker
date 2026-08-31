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
endif()

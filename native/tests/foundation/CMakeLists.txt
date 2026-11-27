cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0176)
    cmake_policy(SET CMP0176 NEW)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NickelFoundationTests LANGUAGES CXX)
    enable_testing()
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/msvc_showincludes.py"
                    "${CMAKE_CXX_COMPILER}" "${CMAKE_CURRENT_BINARY_DIR}"
            RESULT_VARIABLE NICKEL_INCLUDE_PROBE_RESULT
            OUTPUT_VARIABLE NICKEL_INCLUDE_PREFIX
            ERROR_VARIABLE NICKEL_INCLUDE_PROBE_ERROR
            ENCODING UTF-8)
        if(NOT NICKEL_INCLUDE_PROBE_RESULT EQUAL 0)
            message(FATAL_ERROR "MSVC include-prefix probe failed: ${NICKEL_INCLUDE_PROBE_ERROR}")
        endif()
        string(REGEX REPLACE "[\r\n]+$" "" NICKEL_INCLUDE_PREFIX "${NICKEL_INCLUDE_PREFIX}")
        set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${NICKEL_INCLUDE_PREFIX}")
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${NICKEL_INCLUDE_PREFIX}")
    endif()
    get_filename_component(NICKEL_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    find_package(nlohmann_json 3.11 CONFIG QUIET)
    if(NOT TARGET nlohmann_json::nlohmann_json)
        set(NICKEL_JSON_INCLUDE "" CACHE PATH "Verified nlohmann/json include directory")
        if(NOT EXISTS "${NICKEL_JSON_INCLUDE}/nlohmann/json.hpp")
            message(FATAL_ERROR "Supply CMAKE_PREFIX_PATH or NICKEL_JSON_INCLUDE from the verified shared JSON toolchain")
        endif()
        add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
        set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${NICKEL_JSON_INCLUDE}")
    endif()
    set(NICKEL_FOUNDATION_COMMON_ROOT "${NICKEL_NATIVE_ROOT}" CACHE PATH "Native include root containing canonical core/common")
    if(EXISTS "${NICKEL_FOUNDATION_COMMON_ROOT}/core/common/CMakeLists.txt")
        add_subdirectory("${NICKEL_FOUNDATION_COMMON_ROOT}/core/common" "${CMAKE_CURRENT_BINARY_DIR}/common")
        message(STATUS "Using canonical nickel_common from ${NICKEL_FOUNDATION_COMMON_ROOT}")
    else()
        add_library(nickel_common INTERFACE)
        target_include_directories(nickel_common INTERFACE "${CMAKE_CURRENT_LIST_DIR}/support")
        target_link_libraries(nickel_common INTERFACE nlohmann_json::nlohmann_json)
        target_compile_features(nickel_common INTERFACE cxx_std_20)
        message(STATUS "Using foundation-only common mirror; production common is absent")
    endif()
    add_subdirectory("${NICKEL_NATIVE_ROOT}/core/config" "${CMAKE_CURRENT_BINARY_DIR}/foundation")
    if(EXISTS "${NICKEL_FOUNDATION_COMMON_ROOT}/core/common/CMakeLists.txt")
        target_include_directories(nickel_foundation BEFORE PUBLIC "${NICKEL_FOUNDATION_COMMON_ROOT}")
    endif()
endif()

add_executable(nickel_foundation_mock_child mock_child.cpp)
target_link_libraries(nickel_foundation_mock_child PRIVATE nickel_foundation)
add_executable(nickel_foundation_tests foundation_tests.cpp)
target_link_libraries(nickel_foundation_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_adversarial_tests atomic_adversarial.cpp)
target_link_libraries(nickel_foundation_adversarial_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_process_tests process_contract.cpp)
target_link_libraries(nickel_foundation_process_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_gpu_preferences_tests gpu_preferences.cpp)
target_link_libraries(nickel_foundation_gpu_preferences_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_long_paths_tests long_paths.cpp)
target_link_libraries(nickel_foundation_long_paths_tests PRIVATE nickel_foundation)
get_filename_component(NICKEL_FOUNDATION_TEST_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(nickel_foundation_vault_fallback_impl OBJECT "${NICKEL_FOUNDATION_TEST_NATIVE_ROOT}/core/config/vault.cpp")
target_link_libraries(nickel_foundation_vault_fallback_impl PRIVATE nickel_foundation)
target_compile_definitions(nickel_foundation_vault_fallback_impl PRIVATE
    atomic_create_file=foundation_test_atomic_create_file)
add_executable(nickel_foundation_vault_fallback_tests vault_fallback.cpp
    $<TARGET_OBJECTS:nickel_foundation_vault_fallback_impl>)
target_link_libraries(nickel_foundation_vault_fallback_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_stream_copy_tests stream_copy.cpp)
target_link_libraries(nickel_foundation_stream_copy_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_stream_copy_fault_tests stream_copy_faults.cpp stream_copy_fault_impl.cpp)
target_link_libraries(nickel_foundation_stream_copy_fault_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_conditional_tests conditional_write.cpp)
target_link_libraries(nickel_foundation_conditional_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_conditional_fault_tests conditional_write_faults.cpp conditional_write_fault_impl.cpp)
target_link_libraries(nickel_foundation_conditional_fault_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_stop_compat_tests stop_compat_tests.cpp)
target_link_libraries(nickel_foundation_stop_compat_tests PRIVATE nickel_foundation)
add_executable(nickel_foundation_stop_compat_fallback_tests stop_compat_fallback_tests.cpp)
# The fallback binary exercises the in-project portable implementation; it must
# not link nickel_foundation (whose objects follow the standard path) to keep a
# single definition of the inline stop-token classes per binary. The support
# mirror provides core/common only when the canonical tree is absent.
get_filename_component(NICKEL_STOP_COMPAT_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
target_include_directories(nickel_foundation_stop_compat_fallback_tests PRIVATE
    "${NICKEL_STOP_COMPAT_NATIVE_ROOT}" "${CMAKE_CURRENT_LIST_DIR}/support")
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(nickel_foundation_stop_compat_fallback_tests PRIVATE nlohmann_json::nlohmann_json)
endif()
target_compile_definitions(nickel_foundation_stop_compat_fallback_tests PRIVATE NICKEL_FORCE_PORTABLE_CANCELLATION)
add_executable(nickel_foundation_large_copy_probe large_stream_copy_probe.cpp)
target_link_libraries(nickel_foundation_large_copy_probe PRIVATE nickel_foundation)
if(WIN32)
    target_compile_definitions(nickel_foundation_large_copy_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(nickel_foundation_gpu_preferences_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(nickel_foundation_large_copy_probe PRIVATE psapi)
endif()
if(MINGW)
    target_link_options(nickel_foundation_mock_child PRIVATE -municode)
    target_link_options(nickel_foundation_large_copy_probe PRIVATE -municode)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NICKEL_FOUNDATION_FIXTURES "${CMAKE_CURRENT_BINARY_DIR}/fixtures.json")
add_custom_command(OUTPUT "${NICKEL_FOUNDATION_FIXTURES}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/make_fixtures.py" "${NICKEL_FOUNDATION_FIXTURES}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/make_fixtures.py" VERBATIM)
add_custom_target(nickel_foundation_fixtures DEPENDS "${NICKEL_FOUNDATION_FIXTURES}")
add_dependencies(nickel_foundation_tests nickel_foundation_fixtures)
target_compile_definitions(nickel_foundation_tests PRIVATE NICKEL_FOUNDATION_FIXTURES="${NICKEL_FOUNDATION_FIXTURES}")
if(MSVC)
    foreach(target nickel_foundation_mock_child nickel_foundation_tests nickel_foundation_adversarial_tests
                   nickel_foundation_process_tests nickel_foundation_gpu_preferences_tests nickel_foundation_long_paths_tests
                   nickel_foundation_vault_fallback_impl nickel_foundation_vault_fallback_tests
                   nickel_foundation_stream_copy_tests nickel_foundation_stream_copy_fault_tests
                   nickel_foundation_conditional_tests nickel_foundation_conditional_fault_tests
                   nickel_foundation_large_copy_probe)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endforeach()
endif()
add_test(NAME foundation.compatibility COMMAND nickel_foundation_tests "$<TARGET_FILE:nickel_foundation_mock_child>")
add_test(NAME foundation.filesystem_adversarial COMMAND nickel_foundation_adversarial_tests)
add_test(NAME foundation.vault_fallback COMMAND nickel_foundation_vault_fallback_tests)
add_test(NAME foundation.process_contract COMMAND nickel_foundation_process_tests "$<TARGET_FILE:nickel_foundation_mock_child>")
add_test(NAME foundation.gpu_preferences COMMAND nickel_foundation_gpu_preferences_tests "$<TARGET_FILE:nickel_foundation_mock_child>")
set_tests_properties(foundation.gpu_preferences PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "foundation;gpu")
add_test(NAME foundation.long_paths COMMAND nickel_foundation_long_paths_tests "$<TARGET_FILE:nickel_foundation_mock_child>")
add_test(NAME foundation.stream_copy COMMAND nickel_foundation_stream_copy_tests)
add_test(NAME foundation.stream_copy_faults COMMAND nickel_foundation_stream_copy_fault_tests)
add_test(NAME foundation.conditional_write COMMAND nickel_foundation_conditional_tests)
add_test(NAME foundation.conditional_write_faults COMMAND nickel_foundation_conditional_fault_tests)
add_test(NAME foundation.stop_compat COMMAND nickel_foundation_stop_compat_tests)
add_test(NAME foundation.stop_compat_fallback COMMAND nickel_foundation_stop_compat_fallback_tests)
set_tests_properties(foundation.stop_compat foundation.stop_compat_fallback PROPERTIES TIMEOUT 120)
set_tests_properties(foundation.compatibility foundation.filesystem_adversarial foundation.vault_fallback
                     foundation.process_contract foundation.long_paths foundation.stream_copy
                     foundation.stream_copy_faults foundation.conditional_write
                     foundation.conditional_write_faults PROPERTIES TIMEOUT 120)

add_test(NAME foundation.large_copy_small
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_large_stream_copy.py"
            --probe "$<TARGET_FILE:nickel_foundation_large_copy_probe>"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}" --mode small)
add_test(NAME foundation.large_copy_budget
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_large_stream_copy.py"
            --probe "$<TARGET_FILE:nickel_foundation_large_copy_probe>"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}" --mode budget)
set_tests_properties(foundation.large_copy_small foundation.large_copy_budget
    PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS "foundation;streaming-copy")
option(NICKEL_FOUNDATION_ENABLE_LARGE_COPY_TESTS "Opt in to physical >4 GiB copy on an explicit scratch volume" OFF)
set(NICKEL_FOUNDATION_LARGE_COPY_SCRATCH_ROOT "" CACHE PATH "Existing disposable scratch root for opted-in large-copy tests")
if(NICKEL_FOUNDATION_ENABLE_LARGE_COPY_TESTS)
    if(NOT IS_ABSOLUTE "${NICKEL_FOUNDATION_LARGE_COPY_SCRATCH_ROOT}" OR
       NOT IS_DIRECTORY "${NICKEL_FOUNDATION_LARGE_COPY_SCRATCH_ROOT}")
        message(FATAL_ERROR "Large-copy opt-in requires an existing absolute NICKEL_FOUNDATION_LARGE_COPY_SCRATCH_ROOT")
    endif()
    add_test(NAME foundation.large_copy_physical
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_large_stream_copy.py"
                --probe "$<TARGET_FILE:nickel_foundation_large_copy_probe>"
                --scratch-root "${NICKEL_FOUNDATION_LARGE_COPY_SCRATCH_ROOT}" --mode large)
    set_tests_properties(foundation.large_copy_physical PROPERTIES
        TIMEOUT 7200 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS "foundation;streaming-copy;large;opt-in")
endif()

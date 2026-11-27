cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NICKEL_PRODUCTION_SMOKE_LINK_PORTAL)
        set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded CACHE STRING "Runtime of the matched production libraries" FORCE)
    endif()
    project(NickelProductionSmoke LANGUAGES CXX)
    include(CTest)
    if(NICKEL_PRODUCTION_SMOKE_LINK_PORTAL)
        include("${CMAKE_CURRENT_LIST_DIR}/link_generation87.cmake")
    endif()
endif()

if(NOT TARGET Nickel::AppServices)
    message(FATAL_ERROR "Include this existing-matrix harness in the matched production graph; no mock or guessed imported libraries are accepted")
endif()
find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui Widgets)

set(NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256 "" CACHE STRING "Exact manifest of the matched production source/library graph")
string(LENGTH "${NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256}" _pin_length)
if(NOT _pin_length EQUAL 64 OR NOT NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256 must be a full lowercase SHA256, not an inherited generation prefix")
endif()
option(NICKEL_PRODUCTION_SMOKE_SAME_GRAPH "Bind the acceptance consumer to the current production build graph" OFF)
set(NICKEL_PRODUCTION_SMOKE_SOURCE_COMMIT "" CACHE STRING "Full immutable production Git commit")
set(NICKEL_PRODUCTION_SMOKE_HARNESS_SHA256 "" CACHE STRING "Full raw owned acceptance source-delta manifest SHA256")
get_filename_component(_smoke_harness_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NICKEL_PRODUCTION_SMOKE_OWNER_ROOT "${_smoke_harness_root}" CACHE PATH "Exclusive evidence worktree, not a shared builder root")
if(NICKEL_PRODUCTION_SMOKE_SAME_GRAPH)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR NICKEL_PRODUCTION_SMOKE_LINK_PORTAL)
        message(FATAL_ERROR "Current Pd same-graph acceptance must be included in the actual production graph, never the old87 imports")
    endif()
    string(LENGTH "${NICKEL_PRODUCTION_SMOKE_SOURCE_COMMIT}" _commit_length)
    string(LENGTH "${NICKEL_PRODUCTION_SMOKE_HARNESS_SHA256}" _harness_length)
    if(NOT _commit_length EQUAL 40 OR NOT NICKEL_PRODUCTION_SMOKE_SOURCE_COMMIT MATCHES "^[0-9a-f]+$"
        OR NOT _harness_length EQUAL 64 OR NOT NICKEL_PRODUCTION_SMOKE_HARNESS_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Current Pd requires the full production commit and the distinct raw harness manifest hash")
    endif()
endif()
qt_add_executable(nickel_production_smoke_existing_matrix MANUAL_FINALIZATION
    production_main.cpp suite_cases.hpp qt_probe.hpp same_graph_inputs.hpp fixture_inputs.hpp
)
target_compile_features(nickel_production_smoke_existing_matrix PRIVATE cxx_std_20)
target_link_libraries(nickel_production_smoke_existing_matrix PRIVATE Nickel::AppServices Qt6::Widgets)
target_compile_definitions(nickel_production_smoke_existing_matrix PRIVATE
    NICKEL_SMOKE_SOURCE_SHA256="${NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256}"
    NICKEL_SMOKE_PORTAL_SHA256="${NICKEL_PRODUCTION_SMOKE_PORTAL_SHA256}"
    NICKEL_SMOKE_OWNER_ROOT="${NICKEL_PRODUCTION_SMOKE_OWNER_ROOT}"
    NICKEL_SMOKE_HARNESS_ROOT="${_smoke_harness_root}"
)
if(NICKEL_PRODUCTION_SMOKE_SAME_GRAPH)
    target_compile_definitions(nickel_production_smoke_existing_matrix PRIVATE
        NICKEL_SMOKE_SAME_GRAPH=1
        NICKEL_SMOKE_SOURCE_COMMIT="${NICKEL_PRODUCTION_SMOKE_SOURCE_COMMIT}"
        NICKEL_SMOKE_HARNESS_SHA256="${NICKEL_PRODUCTION_SMOKE_HARNESS_SHA256}"
    )
endif()
if(MSVC)
    target_compile_options(nickel_production_smoke_existing_matrix PRIVATE /W4 /WX /utf-8 /EHsc /UNDEBUG)
    if(NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST)
        file(SHA256 "${NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST}" _windows_manifest_sha)
        if(NOT _windows_manifest_sha STREQUAL NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST_SHA256)
            message(FATAL_ERROR "The bound production long-path manifest changed")
        endif()
        target_link_options(nickel_production_smoke_existing_matrix PRIVATE
            "/MANIFEST:EMBED" "/MANIFESTINPUT:${NICKEL_PRODUCTION_SMOKE_WINDOWS_MANIFEST}"
        )
    else()
        message(FATAL_ERROR "The Windows matrix requires the matched production long-path manifest and full SHA256")
    endif()
else()
    target_compile_options(nickel_production_smoke_existing_matrix PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
endif()
if(NOT TARGET Qt6::QOffscreenIntegrationPlugin)
    message(FATAL_ERROR "The production smoke matrix requires the actual Qt 6.8.3 offscreen platform plugin")
endif()
qt_import_plugins(nickel_production_smoke_existing_matrix INCLUDE Qt6::QOffscreenIntegrationPlugin)
qt_finalize_executable(nickel_production_smoke_existing_matrix)

# Same-graph --fixtures binds an immutable template; runtime clones beside each fresh case output.
option(NICKEL_PRODUCTION_SMOKE_REGISTER_TEST "Register this owned matrix in the real CTest graph" ${BUILD_TESTING})
if(BUILD_TESTING AND NICKEL_PRODUCTION_SMOKE_REGISTER_TEST)
    foreach(_input NICKEL_PRODUCTION_SMOKE_VERIFIED_FREEZE NICKEL_PRODUCTION_SMOKE_FIXTURE_MANIFEST)
        if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
            message(FATAL_ERROR "${_input} must declare the exact new owner-local runtime input")
        endif()
        if(NOT NICKEL_PRODUCTION_SMOKE_SAME_GRAPH AND NOT EXISTS "${${_input}}")
            message(FATAL_ERROR "${_input} must identify an existing matched old-generation input")
        endif()
    endforeach()
    if(NOT NICKEL_PRODUCTION_SMOKE_CASE_OUTPUT OR EXISTS "${NICKEL_PRODUCTION_SMOKE_CASE_OUTPUT}")
        message(FATAL_ERROR "NICKEL_PRODUCTION_SMOKE_CASE_OUTPUT must be an unused owner-local evidence file")
    endif()
    add_test(NAME nickel_production_smoke_existing_matrix
        COMMAND nickel_production_smoke_existing_matrix
            --freeze "${NICKEL_PRODUCTION_SMOKE_VERIFIED_FREEZE}"
            --fixtures "${NICKEL_PRODUCTION_SMOKE_FIXTURE_MANIFEST}"
            --matrix "${CMAKE_CURRENT_LIST_DIR}/matrix.json"
            --manifest-sha256 "${NICKEL_PRODUCTION_SMOKE_SOURCE_SHA256}"
            --output "${NICKEL_PRODUCTION_SMOKE_CASE_OUTPUT}"
    )
    set_tests_properties(nickel_production_smoke_existing_matrix PROPERTIES
        TIMEOUT 180
        RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

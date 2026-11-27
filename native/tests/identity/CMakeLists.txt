cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PalladiumIdentityControlledTests LANGUAGES CXX)
    include(CTest)
    set(NICKEL_IDENTITY_CONTROLLED_ONLY ON CACHE BOOL "Standalone controlled identity harness")
    set(NICKEL_IDENTITY_SHARED_NATIVE_ROOT "" CACHE PATH "Read-only actual owner native include/source root")
    option(NICKEL_IDENTITY_TEST_TRANSPORT_RUNTIME "Build actual transport target for identity integration tests" OFF)
    if(NICKEL_IDENTITY_TEST_TRANSPORT_RUNTIME AND NOT NICKEL_IDENTITY_SHARED_NATIVE_ROOT)
        message(FATAL_ERROR "Native identity verification requires a complete actual owner source root")
    endif()
    if(NICKEL_IDENTITY_SHARED_NATIVE_ROOT)
        foreach(export IN ITEMS
                core/common/CMakeLists.txt
                core/config/CMakeLists.txt
                core/config/repository.hpp
                core/config/vault.hpp
                core/config/conditional_file.hpp
                core/config/conditional_file_impl.inc
                core/config/atomic_file.cpp
                core/config/path_guard.hpp
                core/network/CMakeLists.txt
                core/network/http.hpp)
            if(NOT EXISTS "${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}/${export}")
                message(FATAL_ERROR "Matched actual owner export is missing: ${export}")
            endif()
        endforeach()
        if(NICKEL_IDENTITY_TEST_TRANSPORT_RUNTIME)
            foreach(export IN ITEMS
                    core/tasks/CMakeLists.txt
                    core/tasks/secure_fs.hpp
                    core/archive/CMakeLists.txt
                    core/archive/archive.hpp)
                if(NOT EXISTS "${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}/${export}")
                    message(FATAL_ERROR "Actual native filesystem/archive export is missing: ${export}")
                endif()
            endforeach()
        endif()
    endif()
    if(NOT TARGET nlohmann_json::nlohmann_json)
        find_package(nlohmann_json 3.11.3 CONFIG QUIET)
        if(NOT TARGET nlohmann_json::nlohmann_json)
            find_path(NICKEL_IDENTITY_JSON_INCLUDE nlohmann/json.hpp
                PATHS ${CMAKE_CURRENT_LIST_DIR}/.deps
                DOC "Fixed-version or shared nlohmann JSON include root"
                REQUIRED)
            add_library(nickel_identity_json INTERFACE)
            target_include_directories(nickel_identity_json INTERFACE ${NICKEL_IDENTITY_JSON_INCLUDE})
            add_library(nlohmann_json::nlohmann_json ALIAS nickel_identity_json)
        endif()
    endif()
    function(nickel_identity_add_common)
        if(NICKEL_IDENTITY_SHARED_NATIVE_ROOT)
            add_subdirectory("${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}/core/common"
                             "${CMAKE_CURRENT_BINARY_DIR}/nickel_common")
        else()
            get_filename_component(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
            add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../core/common
                             ${CMAKE_CURRENT_BINARY_DIR}/nickel_common)
        endif()
    endfunction()
    nickel_identity_add_common()
    if(NICKEL_IDENTITY_SHARED_NATIVE_ROOT)
        add_subdirectory("${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}/core/config"
                         "${CMAKE_CURRENT_BINARY_DIR}/nickel_foundation")
        if(NICKEL_IDENTITY_TEST_TRANSPORT_RUNTIME)
            add_subdirectory("${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}/core/network"
                             "${CMAKE_CURRENT_BINARY_DIR}/nickel_transport")
        endif()
    endif()
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../core/accounts
                     ${CMAKE_CURRENT_BINARY_DIR}/nickel_identity)
endif()

set(nickel_identity_require_native_default ON)
if(NICKEL_IDENTITY_CONTROLLED_ONLY)
    set(nickel_identity_require_native_default OFF)
endif()
option(NICKEL_IDENTITY_REQUIRE_NATIVE_TESTS "Require source-built Foundation/transport and native identity fixtures"
       ${nickel_identity_require_native_default})
if(NICKEL_IDENTITY_REQUIRE_NATIVE_TESTS)
    if(NICKEL_IDENTITY_CONTROLLED_ONLY)
        message(FATAL_ERROR "Mandatory native identity tests cannot use the controlled-only configuration")
    endif()
    foreach(owner_target IN ITEMS nickel_identity nickel_foundation nickel_transport)
        if(NOT TARGET ${owner_target})
            message(FATAL_ERROR "Mandatory native identity tests require ${owner_target}")
        endif()
        get_target_property(owner_imported ${owner_target} IMPORTED)
        if(owner_imported)
            message(FATAL_ERROR "Mandatory native identity tests require source-built ${owner_target}, not an imported library")
        endif()
    endforeach()
endif()
option(NICKEL_IDENTITY_TEST_DEEP_JOURNAL "Enable strict original deep-journal owner regression" OFF)
if(NICKEL_IDENTITY_TEST_DEEP_JOURNAL AND NOT TARGET nickel_foundation)
    message(FATAL_ERROR "Deep-journal regression requires the actual nickel_foundation target")
endif()


find_package(Threads REQUIRED)
add_executable(nickel_identity_tests
    main.cpp
    accounts_test.cpp
    account_safety_test.cpp
    auth_test.cpp
    common_test.cpp
    lifecycle_test.cpp
    geometry_test.cpp
    builtin_asset_test.cpp
    media_test.cpp
    media_lifecycle_test.cpp
    media_publication_test.cpp
    skin_png_test.cpp
    offline_skin_test.cpp
)
target_compile_features(nickel_identity_tests PRIVATE cxx_std_20)
target_compile_definitions(nickel_identity_tests PRIVATE NICKEL_IDENTITY_FULL_TESTS)
target_link_libraries(nickel_identity_tests PRIVATE
    nickel_identity nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(nickel_identity_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(nickel_identity_tests PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(nickel_identity_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME nickel_identity_controlled COMMAND nickel_identity_tests)
set_tests_properties(nickel_identity_controlled PROPERTIES TIMEOUT 60)
find_program(NICKEL_IDENTITY_NODE_EXECUTABLE NAMES node)
if(NICKEL_IDENTITY_NODE_EXECUTABLE)
    add_test(NAME nickel_identity_original_asset_parity
        COMMAND ${NICKEL_IDENTITY_NODE_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/generate_builtin_heads.cjs --check)
    add_test(NAME nickel_identity_offline_uuid_golden
        COMMAND ${NICKEL_IDENTITY_NODE_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/generate_offline_uuid_golden.cjs --check)
    set_tests_properties(nickel_identity_offline_uuid_golden PROPERTIES
        TIMEOUT 30 LABELS "identity;offline-uuid;source-oracle")
endif()

if(TARGET nickel_foundation)
    add_executable(nickel_identity_foundation_tests foundation_adapter_test.cpp)
    target_link_libraries(nickel_identity_foundation_tests PRIVATE nickel_identity Threads::Threads)
    target_compile_features(nickel_identity_foundation_tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(nickel_identity_foundation_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(nickel_identity_foundation_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME nickel_identity_foundation_adapter
        COMMAND nickel_identity_foundation_tests "${CMAKE_CURRENT_LIST_DIR}/.build-fixtures")
    set_tests_properties(nickel_identity_foundation_adapter PROPERTIES TIMEOUT 60)
    add_test(NAME nickel_identity_offline_uuid_repository
        COMMAND nickel_identity_foundation_tests "${CMAKE_CURRENT_LIST_DIR}/.build-fixtures" --offline-uuid-only)
    set_tests_properties(nickel_identity_offline_uuid_repository PROPERTIES
        TIMEOUT 60 LABELS "identity;foundation;offline-uuid;owner-regression")
    if(NICKEL_IDENTITY_TEST_DEEP_JOURNAL)
        add_test(NAME nickel_identity_foundation_deep_journal
            COMMAND nickel_identity_foundation_tests "${CMAKE_CURRENT_LIST_DIR}/.build-fixtures" --deep-journal)
        set_tests_properties(nickel_identity_foundation_deep_journal PROPERTIES
            TIMEOUT 60 LABELS "foundation;deep-path;owner-regression")
    endif()
endif()
if(NICKEL_IDENTITY_SHARED_NATIVE_ROOT OR TARGET nickel_transport)
    add_executable(nickel_identity_transport_contract transport_contract_test.cpp)
    target_link_libraries(nickel_identity_transport_contract PRIVATE nickel_identity)
    target_compile_features(nickel_identity_transport_contract PRIVATE cxx_std_20)
    if(NICKEL_IDENTITY_SHARED_NATIVE_ROOT)
        target_include_directories(nickel_identity_transport_contract PRIVATE "${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}")
    endif()
    if(MSVC)
        target_compile_options(nickel_identity_transport_contract PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(nickel_identity_transport_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME nickel_identity_transport_request_contract COMMAND nickel_identity_transport_contract)
    if(NOT TARGET nickel_transport)
        add_library(nickel_identity_transport_adapter_compile OBJECT ../../core/accounts/transport_adapter.cpp)
        target_link_libraries(nickel_identity_transport_adapter_compile PRIVATE nickel_identity)
        target_include_directories(nickel_identity_transport_adapter_compile PRIVATE "${NICKEL_IDENTITY_SHARED_NATIVE_ROOT}")
        if(MSVC)
            target_compile_options(nickel_identity_transport_adapter_compile PRIVATE /W4 /WX /permissive- /utf-8)
        else()
            target_compile_options(nickel_identity_transport_adapter_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
endif()

if(TARGET nickel_transport)
    add_executable(nickel_identity_transport_tests transport_adapter_test.cpp)
    target_compile_features(nickel_identity_transport_tests PRIVATE cxx_std_20)
    target_link_libraries(nickel_identity_transport_tests PRIVATE nickel_identity Threads::Threads)
    if(WIN32)
        target_compile_definitions(nickel_identity_transport_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(nickel_identity_transport_tests PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(nickel_identity_transport_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(nickel_identity_transport_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME nickel_identity_transport_runtime COMMAND nickel_identity_transport_tests)
    set_tests_properties(nickel_identity_transport_runtime PROPERTIES TIMEOUT 30)
endif()

if(TARGET nickel_transport AND TARGET nickel_foundation)
    add_executable(nickel_identity_offline_native_tests offline_skin_native_test.cpp)
    target_compile_features(nickel_identity_offline_native_tests PRIVATE cxx_std_20)
    target_link_libraries(nickel_identity_offline_native_tests PRIVATE nickel_identity Threads::Threads)
    if(MSVC)
        target_compile_options(nickel_identity_offline_native_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(nickel_identity_offline_native_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME nickel_identity_offline_native
        COMMAND nickel_identity_offline_native_tests "${CMAKE_CURRENT_LIST_DIR}/.build-fixtures")
    add_test(NAME nickel_identity_offline_native_options
        COMMAND nickel_identity_offline_native_tests "${CMAKE_CURRENT_LIST_DIR}/.build-fixtures" --options-only)
    set_tests_properties(nickel_identity_offline_native nickel_identity_offline_native_options PROPERTIES
        TIMEOUT 60 LABELS "identity;native;owner-regression")
endif()

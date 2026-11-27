cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
get_filename_component(NICKEL_WORKTREE "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(NICKEL_MIRROR "${NICKEL_WORKTREE}/.snow/native-shared")
set(NICKEL_NATIVE "${NICKEL_MIRROR}/native")
file(READ "${NICKEL_NATIVE}/metadata.json" _nickel_metadata)
string(JSON _nickel_version GET "${_nickel_metadata}" version)
project(nickel_application_composition VERSION "${_nickel_version}" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(ZLIB_USE_STATIC_LIBS ON)
set(NICKEL_SHARED_NATIVE_ROOT "${NICKEL_NATIVE}" CACHE PATH "Authoritative domain snapshot" FORCE)
set(NICKEL_COMMUNITY_QT_ADAPTER ON CACHE BOOL "Use the production Qt document adapter" FORCE)
include("${NICKEL_MIRROR}/cmake/NickelDependencies.cmake")
find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui Widgets Svg Test)
include("${NICKEL_MIRROR}/cmake/NickelQtDependencies.cmake")
nickel_configure_qt_dependencies()
add_subdirectory("${NICKEL_NATIVE}/core/common" "domains/common")
add_subdirectory("${NICKEL_NATIVE}/core/config" "domains/foundation")
add_subdirectory("${NICKEL_NATIVE}/core/network" "domains/transport")
add_subdirectory("${NICKEL_NATIVE}/core/minecraft" "domains/minecraft")
add_subdirectory("${NICKEL_NATIVE}/core/instances" "domains/instances")
get_target_property(_nickel_instances_sources nickel_instances SOURCES)
set(_nickel_junk_source "${NICKEL_NATIVE}/core/diagnostics/junk.cpp")
if(NOT _nickel_junk_source IN_LIST _nickel_instances_sources)
    target_sources(nickel_instances PRIVATE "${_nickel_junk_source}")
endif()
add_subdirectory("${NICKEL_NATIVE}/core/accounts" "domains/identity")
add_subdirectory("${NICKEL_NATIVE}/core/resources" "domains/community")

foreach(_field displayName applicationId executableName license)
    string(JSON _nickel_${_field} GET "${_nickel_metadata}" "${_field}")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/native/app")
configure_file("${NICKEL_NATIVE}/app/BuildInfo.hpp.in" "${CMAKE_BINARY_DIR}/native/app/BuildInfo.hpp" @ONLY)

add_library(nickel_application_composed STATIC
    "${NICKEL_WORKTREE}/native/app/AppServices.hpp"
    "${NICKEL_WORKTREE}/native/app/AppServices.cpp"
    "${NICKEL_WORKTREE}/native/app/application_state.hpp"
    "${NICKEL_WORKTREE}/native/app/application_state.cpp"
    "${NICKEL_WORKTREE}/native/app/service/request_registry.cpp"
    "${NICKEL_WORKTREE}/native/app/service/mutation_gate.cpp"
    "${NICKEL_WORKTREE}/native/app/service/public_dto.cpp"
    "${NICKEL_WORKTREE}/native/app/service/snapshots.cpp"
    "${NICKEL_WORKTREE}/native/app/service/state_queries.cpp"
    "${NICKEL_WORKTREE}/native/app/service/community_queries.cpp"
    "${NICKEL_WORKTREE}/native/app/service/action_dispatch.cpp"
    "${NICKEL_WORKTREE}/native/app/service/accounts_actions.cpp"
    "${NICKEL_WORKTREE}/native/app/service/operation_scheduler.cpp"
    "${NICKEL_WORKTREE}/native/app/service/operation_queue.hpp"
    "${NICKEL_WORKTREE}/native/app/service/startup_actions.cpp"
    "${NICKEL_WORKTREE}/native/app/service/diagnostic_actions.cpp"
    "${NICKEL_WORKTREE}/native/app/service/build_metadata.cpp"
    "${NICKEL_WORKTREE}/native/app/service/application_resources.qrc"
    "${NICKEL_NATIVE}/app/service/instance_actions.cpp"
    "${NICKEL_NATIVE}/app/service/resource_actions.cpp"
    "${NICKEL_NATIVE}/app/service/launch_actions.cpp"
    "${NICKEL_NATIVE}/app/WorkerQueue.cpp"
    "${NICKEL_NATIVE}/app/core_adapters.cpp"
    "${NICKEL_NATIVE}/app/settings.cpp"
    "${NICKEL_NATIVE}/ui/UiServices.hpp"
    "${NICKEL_NATIVE}/ui/DecodeImage.cpp"
)
add_library(Nickel::AppServices ALIAS nickel_application_composed)
target_include_directories(nickel_application_composed BEFORE PUBLIC "${NICKEL_NATIVE}" "${NICKEL_WORKTREE}/native")
target_include_directories(nickel_application_composed PRIVATE "${CMAKE_BINARY_DIR}/native" "${CMAKE_BINARY_DIR}/native/app")
target_link_libraries(nickel_application_composed PUBLIC nickel_identity nickel_community_qt
    nickel_instances nickel_minecraft nickel_transport nickel_foundation Qt6::Widgets Qt6::Svg ZLIB::ZLIB)
if(MSVC)
    target_compile_options(nickel_application_composed PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
enable_testing()
include("${NICKEL_WORKTREE}/native/tests/app/production_tests.cmake")

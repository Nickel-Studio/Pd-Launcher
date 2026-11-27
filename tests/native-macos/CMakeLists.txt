cmake_minimum_required(VERSION 3.25)

option(NICKEL_MACOS_HOST_INDEPENDENT_ONLY "Run Python gates without claiming macOS compilation" OFF)
if(NICKEL_MACOS_HOST_INDEPENDENT_ONLY)
    project(NickelMacosDeliveryHostIndependent LANGUAGES NONE)
    enable_testing()
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    add_test(NAME macos_delivery.host_independent_gates
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_delivery.py")
    add_test(NAME macos_delivery.host_independent_ci_gates
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_ci.py")
    add_test(NAME macos_delivery.host_independent_fixture_tools
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_tools.py")
    add_test(NAME macos_delivery.host_independent_install_graph
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/test_install_delivery.py")
    set_tests_properties(macos_delivery.host_independent_gates macos_delivery.host_independent_ci_gates
                        macos_delivery.host_independent_fixture_tools macos_delivery.host_independent_install_graph PROPERTIES
        TIMEOUT 90 LABELS "host-independent;not-apple-runtime")
    return()
endif()

project(NickelMacosDeliveryProbes LANGUAGES CXX)
if(NOT APPLE OR NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR "Real Apple host is required. Windows/Linux syntax or cross compilation is not macOS runtime evidence.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "AppleClang is required for the native macOS delivery probes")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    message(FATAL_ERROR "Choose exactly one native architecture, arm64 or x86_64, per host")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "12.0")
    message(FATAL_ERROR "Explicit macOS 12.0 policy is required; newer runtime success is not minimum-version acceptance")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
enable_testing()
set(NICKEL_MACOS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Exact verified production source root")
get_filename_component(NICKEL_MACOS_SOURCE_ROOT "${NICKEL_MACOS_SOURCE_ROOT}" ABSOLUTE)
if(APPLE)
    # Same Apple libc++ stop-token gate as the root build: force-include the
    # compatibility layer so the family is re-enabled before any probe TU parses
    # <stop_token> or <thread> (see native/core/common/stop_compat.hpp).
    add_compile_options("SHELL:$<$<COMPILE_LANGUAGE:CXX>:-include \"${NICKEL_MACOS_SOURCE_ROOT}/native/core/common/stop_compat.hpp\">")
endif()
find_package(nlohmann_json 3.11.3 EXACT CONFIG REQUIRED)
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL 3.5.9 EXACT REQUIRED COMPONENTS Crypto)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Svg)
if(EXISTS "${Qt6_DIR}/../Qt6Network/Qt6NetworkConfig.cmake")
    message(FATAL_ERROR "The delivery Qt prefix must exclude QtNetwork")
endif()
get_target_property(NICKEL_QT_CORE_TYPE Qt6::Core TYPE)
if(NICKEL_QT_CORE_TYPE STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "macOS delivery requires dynamic Qt frameworks, not a Windows-style static artifact")
endif()
get_target_property(NICKEL_QT_IS_FRAMEWORK Qt6::Core FRAMEWORK)
if(NOT NICKEL_QT_IS_FRAMEWORK)
    message(FATAL_ERROR "The locked Qt Core target must be an Apple framework")
endif()

add_subdirectory("${NICKEL_MACOS_SOURCE_ROOT}/native/core/common" "${CMAKE_CURRENT_BINARY_DIR}/common")
add_subdirectory("${NICKEL_MACOS_SOURCE_ROOT}/native/core/config" "${CMAKE_CURRENT_BINARY_DIR}/foundation")
add_executable(nickel_macos_contract_tests contract_tests.cpp)
target_compile_options(nickel_macos_contract_tests PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(nickel_macos_contract_tests PRIVATE "${NICKEL_MACOS_SOURCE_ROOT}/native")
target_link_libraries(nickel_macos_contract_tests PRIVATE nickel_foundation)
add_test(NAME macos_delivery.paths_vault_lifetime
    COMMAND nickel_macos_contract_tests)
set_tests_properties(macos_delivery.paths_vault_lifetime PROPERTIES
    TIMEOUT 90 LABELS "real-apple-only;synthetic-private-roots;production-foundation")

# Consume the real owner's Theme implementation; no substitute production UI or services.
add_executable(nickel_macos_ui_probe MACOSX_BUNDLE
    ui_probe.cpp
    "${NICKEL_MACOS_SOURCE_ROOT}/native/ui/Theme.cpp"
    "${NICKEL_MACOS_SOURCE_ROOT}/native/ui/Theme.hpp")
target_compile_options(nickel_macos_ui_probe PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(nickel_macos_ui_probe PRIVATE "${NICKEL_MACOS_SOURCE_ROOT}/native")
target_link_libraries(nickel_macos_ui_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
set_target_properties(nickel_macos_ui_probe PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "studio.nickel.launcher.macos-probe"
    MACOSX_BUNDLE_BUNDLE_NAME "Native macOS toolkit probe - not the product"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/probe-Info.plist.in")
foreach(NICKEL_MACOS_SCALE IN ITEMS 1 1.25 1.5 2)
    add_test(NAME "macos_delivery.cocoa_theme_font_dpi_${NICKEL_MACOS_SCALE}"
        COMMAND nickel_macos_ui_probe
            --output "${CMAKE_CURRENT_BINARY_DIR}/ui-probe-${NICKEL_MACOS_SCALE}.json")
    set_tests_properties("macos_delivery.cocoa_theme_font_dpi_${NICKEL_MACOS_SCALE}" PROPERTIES
        TIMEOUT 30 LABELS "real-apple-only;native-cocoa;toolkit-probe-not-production-layout"
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa;QT_SCALE_FACTOR=${NICKEL_MACOS_SCALE};QT_ENABLE_HIGHDPI_SCALING=1")
endforeach()

option(NICKEL_MACOS_INCLUDE_OWNER_FOUNDATION_TESTS "Also run the unchanged canonical foundation suites" ON)
if(NICKEL_MACOS_INCLUDE_OWNER_FOUNDATION_TESTS)
    add_subdirectory("${NICKEL_MACOS_SOURCE_ROOT}/native/tests/foundation" "${CMAKE_CURRENT_BINARY_DIR}/owner-foundation-tests")
endif()

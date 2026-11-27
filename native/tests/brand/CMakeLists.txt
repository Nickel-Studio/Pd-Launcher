cmake_minimum_required(VERSION 3.24)

get_filename_component(NICKEL_BRAND_CHECKOUT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NickelBrandResourceTests LANGUAGES CXX)
    enable_testing()
    set(_nickel_asset_oracle_default "${NICKEL_BRAND_CHECKOUT}")
else()
    set(_nickel_asset_oracle_default "")
endif()

set(ASSET_ORACLE_ROOT "${_nickel_asset_oracle_default}" CACHE PATH
    "Explicit trusted original checkout containing src/assets and the historical brand baseline")
if(NOT ASSET_ORACLE_ROOT)
    message(FATAL_ERROR "Brand tests require an explicit trusted ASSET_ORACLE_ROOT in an included build")
endif()
if(NOT IS_ABSOLUTE "${ASSET_ORACLE_ROOT}")
    message(FATAL_ERROR "ASSET_ORACLE_ROOT must be an absolute trusted checkout path")
endif()
get_filename_component(ASSET_ORACLE_ROOT "${ASSET_ORACLE_ROOT}" ABSOLUTE)
foreach(_required IN ITEMS src/assets/mod-placeholder.png src/assets/fonts/nickel-line.woff2
        tests/brand/functional-icons.sha256)
    if(NOT EXISTS "${ASSET_ORACLE_ROOT}/${_required}")
        message(FATAL_ERROR "Brand tests require a trusted ASSET_ORACLE_ROOT: missing ${_required}")
    endif()
endforeach()
foreach(_required IN ITEMS native/tests/brand/functional-map.json native/assets/brand/asset-manifest.json)
    if(NOT EXISTS "${NICKEL_BRAND_CHECKOUT}/${_required}")
        message(FATAL_ERROR "Brand tests require matching test-source brand/map materials: missing ${_required}")
    endif()
endforeach()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Svg)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Original artwork comes from the oracle; brand/map materials stay with the test sources.
# Ninja must see the same canonical configure input as the production app, not an ../../ alias.
get_filename_component(_nickel_functional_template
    "${CMAKE_CURRENT_LIST_DIR}/../../assets/brand/functional-resources.qrc" ABSOLUTE)
file(READ "${_nickel_functional_template}" _nickel_functional_qrc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nickel_functional_template}")
set(_nickel_original_assets "${ASSET_ORACLE_ROOT}/src/assets/")
string(REPLACE "&" "&amp;" _nickel_original_assets "${_nickel_original_assets}")
string(REPLACE "<" "&lt;" _nickel_original_assets "${_nickel_original_assets}")
string(REPLACE ">" "&gt;" _nickel_original_assets "${_nickel_original_assets}")
string(REPLACE ">../../../src/assets/" ">${_nickel_original_assets}"
    _nickel_functional_qrc "${_nickel_functional_qrc}")
set(_nickel_functional_qrc_path "${CMAKE_CURRENT_BINARY_DIR}/nickel_brand_functional_resources.qrc")
file(WRITE "${_nickel_functional_qrc_path}" "${_nickel_functional_qrc}")

add_executable(nickel_brand_resource_test
    brand_resources_test.cpp
    ../../assets/brand/brand.qrc
    "${_nickel_functional_qrc_path}"
)
target_link_libraries(nickel_brand_resource_test PRIVATE Qt6::Core Qt6::Gui Qt6::Svg)
# A static Qt6::Gui imports only the default (windows) platform plugin. The test always runs
# headless, so the offscreen integration must be imported explicitly instead of relying on the
# default plugin set; otherwise QGuiApplication aborts when it cannot find "offscreen".
# The path-contract regression below includes this file in `cmake -P` script mode, where imported
# Qt targets and Qt's own commands do not exist by design; only a real project build enforces them.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    if(NOT TARGET Qt6::QOffscreenIntegrationPlugin)
        message(FATAL_ERROR "nickel.brand.qt-resources requires the Qt offscreen platform plugin")
    endif()
    qt_import_plugins(nickel_brand_resource_test INCLUDE_BY_TYPE platforms Qt6::QOffscreenIntegrationPlugin)
endif()
target_compile_definitions(nickel_brand_resource_test PRIVATE
    NICKEL_BRAND_CHECKOUT="${NICKEL_BRAND_CHECKOUT}"
    ASSET_ORACLE_ROOT="${ASSET_ORACLE_ROOT}"
)
add_test(NAME nickel.brand.qt-resources COMMAND nickel_brand_resource_test)
set_tests_properties(nickel.brand.qt-resources PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "brand;immutable-assets;qt"
)

foreach(_dependency nickel_common nickel_foundation nickel_transport nickel_identity nickel_instances nickel_community nickel_minecraft Nickel::CommunityQt Nickel::Ui)
    if(NOT TARGET "${_dependency}")
        message(FATAL_ERROR "The complete application requires production target ${_dependency}")
    endif()
endforeach()

set(_nickel_app_owner_source "${CMAKE_CURRENT_SOURCE_DIR}")
set(_nickel_asset_source "${CMAKE_SOURCE_DIR}/native/assets/brand")
if(NICKEL_SHARED_NATIVE_ROOT)
    set(_nickel_app_owner_source "${NICKEL_SHARED_NATIVE_ROOT}/app")
    set(_nickel_asset_source "${NICKEL_SHARED_NATIVE_ROOT}/assets/brand")
endif()
set(_nickel_service_sources
    AppServices.cpp
    application_state.cpp
    service/request_registry.cpp
    service/mutation_gate.cpp
    service/public_dto.cpp
    service/snapshots.cpp
    service/state_queries.cpp
    service/community_queries.cpp
    service/action_dispatch.cpp
    service/operation_scheduler.cpp
    service/accounts_actions.cpp
    service/startup_actions.cpp
    service/build_metadata.cpp
    service/trusted_launcher.cpp
    service/launcher_export_services.cpp
    service/instance_actions.cpp
    service/resource_actions.cpp
    service/launch_actions.cpp
    service/diagnostic_actions.cpp
)
set(_nickel_service_headers AppServices.hpp application_state.hpp)
set(_nickel_service_resources service/application_resources.qrc)
foreach(_source IN LISTS _nickel_service_sources _nickel_service_headers _nickel_service_resources)
    if(NOT EXISTS "${_nickel_app_owner_source}/${_source}")
        message(FATAL_ERROR "Missing production application source: ${_nickel_app_owner_source}/${_source}; synchronize the application owner, not a mock")
    endif()
endforeach()
list(TRANSFORM _nickel_service_sources PREPEND "${_nickel_app_owner_source}/")
list(TRANSFORM _nickel_service_headers PREPEND "${_nickel_app_owner_source}/")
list(TRANSFORM _nickel_service_resources PREPEND "${_nickel_app_owner_source}/")
if(NICKEL_SHARED_NATIVE_ROOT)
    file(READ "${_nickel_app_owner_source}/service/application_resources.qrc" _nickel_application_qrc)
    set(_nickel_changelog_xml "${CMAKE_SOURCE_DIR}/CHANGELOG.json")
    string(REPLACE "&" "&amp;" _nickel_changelog_xml "${_nickel_changelog_xml}")
    string(REPLACE "<" "&lt;" _nickel_changelog_xml "${_nickel_changelog_xml}")
    string(REPLACE ">" "&gt;" _nickel_changelog_xml "${_nickel_changelog_xml}")
    string(REPLACE "../../../CHANGELOG.json" "${_nickel_changelog_xml}" _nickel_application_qrc "${_nickel_application_qrc}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/application_resources.qrc" "${_nickel_application_qrc}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_nickel_app_owner_source}/service/application_resources.qrc" "${CMAKE_SOURCE_DIR}/CHANGELOG.json")
    set(_nickel_service_resources "${CMAKE_CURRENT_BINARY_DIR}/application_resources.qrc")
endif()

foreach(_field displayName applicationId executableName license versionLabel)
    string(JSON _nickel_${_field} GET "${_nickel_metadata}" "${_field}")
endforeach()
configure_file(BuildInfo.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.hpp" @ONLY)
set(NICKEL_ENTRY_BUILD_INFO_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" PARENT_SCOPE)
include("${CMAKE_SOURCE_DIR}/cmake/NickelPublicBuildConfig.cmake")
nickel_generate_public_build_config("${CMAKE_CURRENT_BINARY_DIR}")

add_library(nickel_app_services STATIC
    WorkerQueue.cpp WorkerQueue.hpp
    core_adapters.cpp core_adapters.hpp
    settings.cpp settings.hpp
    ${_nickel_service_sources} ${_nickel_service_headers} ${_nickel_service_resources}
)
add_library(Nickel::AppServices ALIAS nickel_app_services)
set_target_properties(nickel_app_services PROPERTIES AUTOMOC ON AUTORCC ON)
target_compile_features(nickel_app_services PUBLIC cxx_std_20)
target_include_directories(nickel_app_services PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${_nickel_app_owner_source}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(nickel_app_services PUBLIC
    nickel_common nickel_foundation nickel_transport nickel_identity nickel_instances
    nickel_community nickel_minecraft Nickel::CommunityQt Nickel::Ui
)
if(WIN32)
    target_link_libraries(nickel_app_services PRIVATE version)
endif()
if(WIN32)
    # Compiled notice material: the verified 145 inputs become one RCDATA
    # resource each plus a generated metadata header consumed by the
    # trusted-launcher service. Generation fails closed on any mismatch.
    set(NICKEL_NOTICE_MANIFEST "${CMAKE_SOURCE_DIR}/docs/native/licenses/trusted-launcher-notice-input.json")
    set(NICKEL_NOTICE_INPUT_ROOT "${CMAKE_SOURCE_DIR}/docs/native/licenses/trusted-launcher-notice-inputs")
    set(NICKEL_NOTICE_INPUT_PREFIX "docs/native/licenses/trusted-launcher-notice-inputs/")
    set(NICKEL_NOTICE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    if(NOT EXISTS "${NICKEL_NOTICE_MANIFEST}" OR NOT IS_DIRECTORY "${NICKEL_NOTICE_INPUT_ROOT}")
        message(FATAL_ERROR "Missing trusted-launcher notice inputs under ${CMAKE_SOURCE_DIR}/docs/native/licenses")
    endif()
    # Dependencies are enumerated explicitly from the verified manifest. Hidden
    # source discovery is forbidden in the production app build, and a manifest
    # row that is malformed, empty or escapes the verified input root must fail
    # here instead of silently compiling a different notice set.
    file(READ "${NICKEL_NOTICE_MANIFEST}" _nickel_notice_manifest_text)
    string(JSON _nickel_notice_row_count ERROR_VARIABLE _nickel_notice_json_error
        LENGTH "${_nickel_notice_manifest_text}" notices)
    if(_nickel_notice_json_error OR NOT _nickel_notice_row_count GREATER 0)
        message(FATAL_ERROR "Trusted-launcher notice manifest is malformed or has no notice rows: ${NICKEL_NOTICE_MANIFEST} ${_nickel_notice_json_error}")
    endif()
    string(JSON _nickel_notice_declared_count ERROR_VARIABLE _nickel_notice_json_error
        GET "${_nickel_notice_manifest_text}" noticeCount)
    if(_nickel_notice_json_error OR NOT _nickel_notice_declared_count EQUAL _nickel_notice_row_count)
        message(FATAL_ERROR "Trusted-launcher notice manifest noticeCount ${_nickel_notice_declared_count} does not match its ${_nickel_notice_row_count} rows: ${NICKEL_NOTICE_MANIFEST} ${_nickel_notice_json_error}")
    endif()
    set(_nickel_notice_inputs)
    math(EXPR _nickel_notice_last_index "${_nickel_notice_row_count} - 1")
    foreach(_nickel_notice_index RANGE ${_nickel_notice_last_index})
        string(JSON _nickel_notice_relative_path ERROR_VARIABLE _nickel_notice_json_error
            GET "${_nickel_notice_manifest_text}" notices ${_nickel_notice_index} path)
        if(_nickel_notice_json_error OR _nickel_notice_relative_path STREQUAL "")
            message(FATAL_ERROR "Notice row ${_nickel_notice_index} has no usable path in ${NICKEL_NOTICE_MANIFEST}: ${_nickel_notice_json_error}")
        endif()
        if(NOT _nickel_notice_relative_path MATCHES "^${NICKEL_NOTICE_INPUT_PREFIX}[^/]")
            message(FATAL_ERROR "Notice row ${_nickel_notice_index} path is outside the verified notice input root: ${_nickel_notice_relative_path}")
        endif()
        string(FIND "${_nickel_notice_relative_path}" ".." _nickel_notice_escape)
        if(NOT _nickel_notice_escape EQUAL -1 OR IS_ABSOLUTE "${_nickel_notice_relative_path}")
            message(FATAL_ERROR "Notice row ${_nickel_notice_index} path is not a canonical relative path: ${_nickel_notice_relative_path}")
        endif()
        set(_nickel_notice_file "${CMAKE_SOURCE_DIR}/${_nickel_notice_relative_path}")
        if(NOT EXISTS "${_nickel_notice_file}" OR IS_DIRECTORY "${_nickel_notice_file}")
            message(FATAL_ERROR "Missing notice input declared by the manifest: ${_nickel_notice_relative_path}")
        endif()
        list(FIND _nickel_notice_inputs "${_nickel_notice_file}" _nickel_notice_duplicate)
        if(NOT _nickel_notice_duplicate EQUAL -1)
            message(FATAL_ERROR "Duplicate notice input path in the manifest: ${_nickel_notice_relative_path}")
        endif()
        list(APPEND _nickel_notice_inputs "${_nickel_notice_file}")
    endforeach()
    list(LENGTH _nickel_notice_inputs _nickel_notice_input_count)
    if(NOT _nickel_notice_input_count EQUAL _nickel_notice_row_count)
        message(FATAL_ERROR "Resolved ${_nickel_notice_input_count} notice inputs for ${_nickel_notice_row_count} manifest rows")
    endif()
    # The manifest drives the explicit path list, so re-configure when it
    # changes. Notice bytes themselves are tracked by the command DEPENDS below;
    # the root's .gitattributes is the sentinel for tracked root membership.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${NICKEL_NOTICE_MANIFEST}" "${NICKEL_NOTICE_INPUT_ROOT}/.gitattributes")
    add_custom_command(
        OUTPUT "${NICKEL_NOTICE_GENERATED_DIR}/TrustedLauncherBuild.hpp"
               "${NICKEL_NOTICE_GENERATED_DIR}/notice_resources.rc"
               "${NICKEL_NOTICE_GENERATED_DIR}/notice-manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NICKEL_NOTICE_GENERATED_DIR}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/scripts/native/generate_notice_resources.py"
                --repo "${CMAKE_SOURCE_DIR}"
                --manifest "${NICKEL_NOTICE_MANIFEST}"
                --out-dir "${NICKEL_NOTICE_GENERATED_DIR}"
        DEPENDS "${CMAKE_SOURCE_DIR}/scripts/native/generate_notice_resources.py"
                "${CMAKE_SOURCE_DIR}/scripts/native/legal_input_verify.py"
                "${NICKEL_NOTICE_MANIFEST}"
                ${_nickel_notice_inputs}
        COMMENT "Generating verified compiled notice resources"
        VERBATIM)
    add_custom_target(nickel_notice_resources DEPENDS
        "${NICKEL_NOTICE_GENERATED_DIR}/TrustedLauncherBuild.hpp"
        "${NICKEL_NOTICE_GENERATED_DIR}/notice_resources.rc"
        "${NICKEL_NOTICE_GENERATED_DIR}/notice-manifest.json")
    add_dependencies(nickel_app_services nickel_notice_resources)
    target_include_directories(nickel_app_services PRIVATE "${NICKEL_NOTICE_GENERATED_DIR}")
endif()

foreach(_resource brand.qrc functional-resources.qrc platform/app.ico platform/app.icns)
    if(NOT EXISTS "${_nickel_asset_source}/${_resource}")
        message(FATAL_ERROR "Missing canonical application asset: ${_nickel_asset_source}/${_resource}")
    endif()
endforeach()
# Only relocate the immutable oracle asset paths in the generated build input.
file(READ "${_nickel_asset_source}/functional-resources.qrc" _nickel_functional_qrc)
set(_nickel_assets_xml "${CMAKE_SOURCE_DIR}/src/assets")
string(REPLACE "&" "&amp;" _nickel_assets_xml "${_nickel_assets_xml}")
string(REPLACE "<" "&lt;" _nickel_assets_xml "${_nickel_assets_xml}")
string(REPLACE ">" "&gt;" _nickel_assets_xml "${_nickel_assets_xml}")
string(REPLACE "../../../src/assets/" "${_nickel_assets_xml}/" _nickel_functional_qrc "${_nickel_functional_qrc}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc" "${_nickel_functional_qrc}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nickel_asset_source}/functional-resources.qrc")

qt_add_executable(ni-launcher MANUAL_FINALIZATION
    main.cpp
    AcceptancePaths.cpp AcceptancePaths.hpp
    PerformanceApplication.cpp PerformanceApplication.hpp
    PerformanceProbe.cpp PerformanceProbe.hpp
    "${_nickel_asset_source}/brand.qrc"
    "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
)
if(WIN32)
    target_link_libraries(ni-launcher PRIVATE dwmapi gdi32)
endif()
set_target_properties(ni-launcher PROPERTIES
    AUTOMOC ON AUTORCC ON
    OUTPUT_NAME "${_nickel_executableName}"
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "${_nickel_displayName}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${_nickel_applicationId}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
target_link_libraries(ni-launcher PRIVATE Nickel::AppServices Qt6::Svg)

get_target_property(_nickel_qt_core_type Qt6::Core TYPE)
if(WIN32 AND NOT _nickel_qt_core_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Windows single-EXE builds require actual static Qt, not deployment DLLs")
endif()
if(_nickel_qt_core_type STREQUAL "STATIC_LIBRARY")
    set(_nickel_image_plugins Qt6::QGifPlugin Qt6::QICOPlugin Qt6::QJpegPlugin Qt6::QSvgPlugin Qt6::QWebpPlugin)
    set(_nickel_required_static_plugins ${_nickel_image_plugins} Qt6::QSvgIconPlugin)
    foreach(_plugin IN LISTS _nickel_required_static_plugins)
        if(NOT TARGET "${_plugin}")
            message(FATAL_ERROR "Required static image plugin is missing: ${_plugin}")
        endif()
    endforeach()
    qt_import_plugins(ni-launcher INCLUDE_BY_TYPE imageformats ${_nickel_image_plugins}
        INCLUDE Qt6::QSvgIconPlugin)
    if(WIN32)
        set(_nickel_platform_plugin Qt6::QWindowsIntegrationPlugin)
    elseif(APPLE)
        set(_nickel_platform_plugin Qt6::QCocoaIntegrationPlugin)
    elseif(UNIX)
        set(_nickel_platform_plugin Qt6::QXcbIntegrationPlugin)
    else()
        message(FATAL_ERROR "No supported static desktop platform plugin for ${CMAKE_SYSTEM_NAME}")
    endif()
    if(NOT TARGET "${_nickel_platform_plugin}")
        message(FATAL_ERROR "Required static platform plugin is missing: ${_nickel_platform_plugin}")
    endif()
    qt_import_plugins(ni-launcher INCLUDE_BY_TYPE platforms "${_nickel_platform_plugin}")
elseif(NOT _nickel_qt_core_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "The actual Qt6::Core target must be static or shared")
endif()
if(WIN32)
    enable_language(RC)
    string(REPLACE "." "," _nickel_version_tuple "${PROJECT_VERSION}")
    configure_file(windows.rc.in "${CMAKE_CURRENT_BINARY_DIR}/windows.rc" @ONLY)
    configure_file(windows.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/windows.manifest" @ONLY)
    target_sources(ni-launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/windows.rc"
        "${NICKEL_NOTICE_GENERATED_DIR}/notice_resources.rc")
    add_dependencies(ni-launcher nickel_notice_resources)
    set_property(SOURCE "${NICKEL_NOTICE_GENERATED_DIR}/notice_resources.rc" APPEND PROPERTY
        OBJECT_DEPENDS "${NICKEL_NOTICE_MANIFEST};${_nickel_notice_inputs}")
    if(MSVC)
        # /Brepro replaces wall-clock stamps (PE COFF TimeDateStamp and its .rdata mirror)
        # with a content hash so two identical inputs link to identical bytes.
        target_link_options(ni-launcher PRIVATE /Brepro /MANIFEST:EMBED
            "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/windows.manifest"
            "/MAP:${CMAKE_CURRENT_BINARY_DIR}/${_nickel_executableName}.map")
    endif()
elseif(APPLE)
    target_sources(ni-launcher PRIVATE "${_nickel_asset_source}/platform/app.icns")
    set_source_files_properties("${_nickel_asset_source}/platform/app.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(ni-launcher PROPERTIES MACOSX_BUNDLE_ICON_FILE app.icns)
endif()
if(MSVC)
    target_compile_options(nickel_app_services PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
    target_compile_options(ni-launcher PRIVATE /W4 /WX /utf-8 /EHsc /permissive- /Zc:__cplusplus)
else()
    target_compile_options(nickel_app_services PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ni-launcher PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NICKEL_ENABLE_IPO)
    set_property(TARGET nickel_app_services ni-launcher PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
qt_finalize_executable(ni-launcher)
# Root blocks test-graph installation; retain actual destinations for same-graph File API discovery.
install(TARGETS ni-launcher RUNTIME DESTINATION . BUNDLE DESTINATION .)
if(_nickel_qt_core_type STREQUAL "SHARED_LIBRARY")
    # Shared desktop builds need a deployed native Qt bundle, not static imports.
    # The platform owner still verifies the installed plugin/runtime closure.
    qt_generate_deploy_app_script(TARGET ni-launcher OUTPUT_SCRIPT _nickel_deploy_script)
    install(SCRIPT "${_nickel_deploy_script}")
endif()

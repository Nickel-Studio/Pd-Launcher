cmake_minimum_required(VERSION 3.24)
project(NickelYaml VERSION 0.2.5 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT EXISTS "${LIBYAML_SOURCE_DIR}/include/yaml.h")
    message(FATAL_ERROR "LIBYAML_SOURCE_DIR must identify verified libyaml 0.2.5 source")
endif()
add_library(yaml STATIC
    "${LIBYAML_SOURCE_DIR}/src/api.c"
    "${LIBYAML_SOURCE_DIR}/src/dumper.c"
    "${LIBYAML_SOURCE_DIR}/src/emitter.c"
    "${LIBYAML_SOURCE_DIR}/src/loader.c"
    "${LIBYAML_SOURCE_DIR}/src/parser.c"
    "${LIBYAML_SOURCE_DIR}/src/reader.c"
    "${LIBYAML_SOURCE_DIR}/src/scanner.c"
    "${LIBYAML_SOURCE_DIR}/src/writer.c")
add_library(yaml::yaml ALIAS yaml)
target_include_directories(yaml PUBLIC
    "$<BUILD_INTERFACE:${LIBYAML_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(yaml PUBLIC YAML_DECLARE_STATIC PRIVATE
    HAVE_CONFIG_H=0 YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5
    YAML_VERSION_STRING="0.2.5")
if(MSVC)
    target_compile_definitions(yaml PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(yaml PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS yaml EXPORT yamlTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${LIBYAML_SOURCE_DIR}/include/yaml.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT yamlTargets NAMESPACE yaml:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/yaml")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/yamlConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/yamlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/yamlConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/yaml")
install(FILES "${LIBYAML_SOURCE_DIR}/License" DESTINATION "${CMAKE_INSTALL_DATADIR}/nickel/licenses/yaml")

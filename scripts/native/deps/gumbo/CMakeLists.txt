cmake_minimum_required(VERSION 3.24)
project(NickelGumbo VERSION 0.10.1 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT EXISTS "${GUMBO_SOURCE_DIR}/src/gumbo.h")
    message(FATAL_ERROR "GUMBO_SOURCE_DIR must identify verified Gumbo 0.10.1 source")
endif()
add_library(gumbo STATIC
    "${GUMBO_SOURCE_DIR}/src/attribute.c"
    "${GUMBO_SOURCE_DIR}/src/char_ref.c"
    "${GUMBO_SOURCE_DIR}/src/error.c"
    "${GUMBO_SOURCE_DIR}/src/parser.c"
    "${GUMBO_SOURCE_DIR}/src/string_buffer.c"
    "${GUMBO_SOURCE_DIR}/src/string_piece.c"
    "${GUMBO_SOURCE_DIR}/src/tag.c"
    "${GUMBO_SOURCE_DIR}/src/tokenizer.c"
    "${GUMBO_SOURCE_DIR}/src/utf8.c"
    "${GUMBO_SOURCE_DIR}/src/util.c"
    "${GUMBO_SOURCE_DIR}/src/vector.c")
add_library(gumbo::gumbo ALIAS gumbo)
target_include_directories(gumbo PUBLIC
    "$<BUILD_INTERFACE:${GUMBO_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(MSVC)
    target_include_directories(gumbo PRIVATE "${GUMBO_SOURCE_DIR}/visualc/include")
    target_compile_definitions(gumbo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS gumbo EXPORT gumboTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${GUMBO_SOURCE_DIR}/src/gumbo.h" "${GUMBO_SOURCE_DIR}/src/tag_enum.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT gumboTargets NAMESPACE gumbo:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gumbo")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/gumbo-config-version.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/gumbo-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/gumbo-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/gumbo")
install(FILES "${GUMBO_SOURCE_DIR}/COPYING" DESTINATION "${CMAKE_INSTALL_DATADIR}/nickel/licenses/gumbo")

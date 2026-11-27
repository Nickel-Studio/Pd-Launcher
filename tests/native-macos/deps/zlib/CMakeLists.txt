cmake_minimum_required(VERSION 3.25)
project(NickelMacosStaticZlib LANGUAGES C)
include(GNUInstallDirs)
if(NOT IS_DIRECTORY "${NICKEL_ZLIB_SOURCE_DIR}")
    message(FATAL_ERROR "Verified zlib 1.3.1 archive root required")
endif()
set(zlib_sources
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)
list(TRANSFORM zlib_sources PREPEND "${NICKEL_ZLIB_SOURCE_DIR}/")
add_library(z STATIC ${zlib_sources})
target_include_directories(z PUBLIC "${NICKEL_ZLIB_SOURCE_DIR}")
target_compile_definitions(z PRIVATE ZLIB_CONST HAVE_UNISTD_H)
set_target_properties(z PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS z ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${NICKEL_ZLIB_SOURCE_DIR}/zlib.h" "${NICKEL_ZLIB_SOURCE_DIR}/zconf.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

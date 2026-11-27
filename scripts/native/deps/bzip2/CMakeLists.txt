cmake_minimum_required(VERSION 3.24)
project(NickelBZip2 VERSION 1.0.8 LANGUAGES C)
include(GNUInstallDirs)
if(NOT IS_DIRECTORY "${BZIP2_SOURCE_DIR}")
    message(FATAL_ERROR "BZIP2_SOURCE_DIR must be a verified bzip2 1.0.8 source directory")
endif()
add_library(bz2 STATIC
    "${BZIP2_SOURCE_DIR}/blocksort.c"
    "${BZIP2_SOURCE_DIR}/huffman.c"
    "${BZIP2_SOURCE_DIR}/crctable.c"
    "${BZIP2_SOURCE_DIR}/randtable.c"
    "${BZIP2_SOURCE_DIR}/compress.c"
    "${BZIP2_SOURCE_DIR}/decompress.c"
    "${BZIP2_SOURCE_DIR}/bzlib.c")
target_include_directories(bz2 PUBLIC
    "$<BUILD_INTERFACE:${BZIP2_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(bz2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS bz2 ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${BZIP2_SOURCE_DIR}/bzlib.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

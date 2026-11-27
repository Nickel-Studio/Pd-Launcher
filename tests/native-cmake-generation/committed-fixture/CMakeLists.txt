cmake_minimum_required(VERSION 3.25)
project(PdCommittedCMakeGenerationRegression LANGUAGES NONE)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "The regression requires the actual Ninja generator")
endif()

set(ASSET_ORACLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
add_subdirectory(native/app)
add_subdirectory(native/tests/brand)

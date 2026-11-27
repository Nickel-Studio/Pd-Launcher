cmake_minimum_required(VERSION 3.24)
if(NOT TARGET nickel_instances)
    message(FATAL_ERROR "Diagnostics sources belong to the nickel_instances target")
endif()
target_sources(nickel_instances PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/logs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/report_export.cpp")

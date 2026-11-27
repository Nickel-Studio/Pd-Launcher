cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NickelTransportTests LANGUAGES CXX)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    enable_testing()
    if(WIN32)
        include("${CMAKE_CURRENT_LIST_DIR}/canonical_foundation.cmake")
    endif()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../core/network" "${CMAKE_CURRENT_BINARY_DIR}/transport")
    target_compile_definitions(nickel_transport PRIVATE NICKEL_TRANSPORT_TEST_HOOKS)
endif()
if(NOT TARGET nickel_transport)
    message(FATAL_ERROR "nickel_transport is required for transport tests")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(nickel_transport_safety safety_test.cpp)
target_link_libraries(nickel_transport_safety PRIVATE nickel_transport)
add_test(NAME transport_safety COMMAND nickel_transport_safety)
set_tests_properties(transport_safety PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../../..")
add_executable(nickel_transport_leaf leaf_test.cpp)
target_link_libraries(nickel_transport_leaf PRIVATE nickel_transport)
set(NICKEL_LEAF_FIXTURE_ARGUMENTS)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_compile_definitions(nickel_transport_leaf PRIVATE NICKEL_TRANSPORT_TEST_HOOKS)
    list(APPEND NICKEL_LEAF_FIXTURE_ARGUMENTS --hooks)
endif()
add_test(NAME transport_guarded_leaf
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/leaf_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_leaf>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/leaf-fixtures"
        ${NICKEL_LEAF_FIXTURE_ARGUMENTS})
set_tests_properties(transport_guarded_leaf PROPERTIES TIMEOUT 100)
add_executable(nickel_transport_tasks task_test.cpp)
target_link_libraries(nickel_transport_tasks PRIVATE nickel_transport)
set(NICKEL_TASK_FIXTURE_ARGUMENTS)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_compile_definitions(nickel_transport_tasks PRIVATE NICKEL_TRANSPORT_TEST_HOOKS)
    list(APPEND NICKEL_TASK_FIXTURE_ARGUMENTS --faults)
endif()
add_test(NAME transport_tasks
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/task_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_tasks>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/task-fixtures"
        ${NICKEL_TASK_FIXTURE_ARGUMENTS})
add_test(NAME transport_file_steps
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/task_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_tasks>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/file-step-fixtures"
        --steps ${NICKEL_TASK_FIXTURE_ARGUMENTS})
set_tests_properties(transport_file_steps PROPERTIES TIMEOUT 120)
add_executable(nickel_transport_zip_index zip_index_test.cpp)
target_link_libraries(nickel_transport_zip_index PRIVATE nickel_transport)
add_test(NAME transport_zip_structure
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/zip_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_zip_index>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/zip-fixtures")
add_executable(nickel_transport_archive archive_test.cpp)
target_link_libraries(nickel_transport_archive PRIVATE nickel_transport)
add_test(NAME transport_archive_runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/archive_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_archive>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/archive-fixtures")
set_tests_properties(transport_archive_runtime PROPERTIES TIMEOUT 160)
add_test(NAME transport_archive_readonly
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/archive_fixture.py"
        --executable "$<TARGET_FILE:nickel_transport_archive>"
        --workspace "${CMAKE_CURRENT_BINARY_DIR}/archive-readonly-fixtures"
        --readonly)
set_tests_properties(transport_archive_readonly PROPERTIES TIMEOUT 100)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/native_tests.cpp")
    add_executable(nickel_transport_tests native_tests.cpp)
    target_link_libraries(nickel_transport_tests PRIVATE nickel_transport)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        target_compile_definitions(nickel_transport_tests PRIVATE NICKEL_TRANSPORT_TEST_HOOKS)
    endif()
    add_test(NAME transport_adversarial
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/socket_fixture.py"
            --executable "$<TARGET_FILE:nickel_transport_tests>"
            --workspace "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    add_test(NAME transport_snapshot_isolation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/socket_fixture.py"
            --executable "$<TARGET_FILE:nickel_transport_tests>"
            --workspace "${CMAKE_CURRENT_BINARY_DIR}/snapshot-fixtures" --snapshots)
    set_tests_properties(transport_adversarial transport_snapshot_isolation PROPERTIES TIMEOUT 180)
endif()
set_tests_properties(transport_tasks transport_zip_structure PROPERTIES TIMEOUT 120)
if(MSVC)
    foreach(target nickel_transport_safety nickel_transport_leaf nickel_transport_tasks nickel_transport_zip_index nickel_transport_archive nickel_transport_tests)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endif()

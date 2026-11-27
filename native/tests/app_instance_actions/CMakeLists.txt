cmake_minimum_required(VERSION 3.25)

foreach(_dependency Nickel::AppServices nickel_transport nickel_foundation nickel_instances)
    if(NOT TARGET "${_dependency}")
        message(FATAL_ERROR "Instance action tests require the real production target ${_dependency}")
    endif()
endforeach()
foreach(_source domain_tests.cpp queue_controls.cpp commit_controls.cpp icon_actions.cpp run_fixture.py)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_source}")
        message(FATAL_ERROR "Missing owned instance action test source: ${CMAKE_CURRENT_LIST_DIR}/${_source}")
    endif()
endforeach()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

add_executable(nickel_app_instance_domain_tests domain_tests.cpp)
add_executable(nickel_app_instance_queue_controls queue_controls.cpp)
add_executable(nickel_app_instance_commit_controls commit_controls.cpp)
add_executable(nickel_app_instance_icon_actions icon_actions.cpp)
set(_instance_targets
    nickel_app_instance_domain_tests
    nickel_app_instance_queue_controls
    nickel_app_instance_commit_controls
    nickel_app_instance_icon_actions
)
foreach(_target IN LISTS _instance_targets)
    target_compile_features("${_target}" PRIVATE cxx_std_20)
    target_link_libraries("${_target}" PRIVATE Nickel::AppServices)
    if(MSVC)
        target_compile_options("${_target}" PRIVATE /W4 /WX /utf-8 /UNDEBUG)
    else()
        target_compile_options("${_target}" PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()
endforeach()
# Root controls use the linked production library, never a second hook-enabled backend.
target_compile_definitions(nickel_app_instance_commit_controls PRIVATE NICKEL_INSTANCE_PRODUCTION_CONTROLS)

if(MSVC)
    set(_instance_manifest_template "${CMAKE_SOURCE_DIR}/native/app/windows.manifest.in")
    if(NOT EXISTS "${_instance_manifest_template}")
        message(FATAL_ERROR "Instance action tests require the canonical production Windows manifest template")
    endif()
    file(READ "${CMAKE_SOURCE_DIR}/native/metadata.json" _instance_metadata)
    string(JSON _nickel_applicationId GET "${_instance_metadata}" applicationId)
    string(JSON _nickel_displayName GET "${_instance_metadata}" displayName)
    configure_file("${_instance_manifest_template}" "${CMAKE_CURRENT_BINARY_DIR}/instance-tests.manifest" @ONLY)
    foreach(_target IN LISTS _instance_targets)
        target_link_options("${_target}" PRIVATE
            /MANIFEST:EMBED
            "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/instance-tests.manifest"
        )
    endforeach()
endif()

set(_instance_workspace "${CMAKE_CURRENT_LIST_DIR}/.build-ctest")
add_test(NAME app_instance_domain
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/run_fixture.py"
        --executable "$<TARGET_FILE:nickel_app_instance_domain_tests>"
        --kind domain --workspace "${_instance_workspace}")
add_test(NAME app_instance_queue
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/run_fixture.py"
        --executable "$<TARGET_FILE:nickel_app_instance_queue_controls>"
        --kind queue --workspace "${_instance_workspace}")
add_test(NAME app_instance_commit
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/run_fixture.py"
        --executable "$<TARGET_FILE:nickel_app_instance_commit_controls>"
        --kind commit --workspace "${_instance_workspace}")
add_test(NAME app_instance_icon
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/run_fixture.py"
        --executable "$<TARGET_FILE:nickel_app_instance_icon_actions>"
        --kind icon --workspace "${_instance_workspace}")
set_tests_properties(app_instance_domain app_instance_queue app_instance_commit app_instance_icon PROPERTIES
    TIMEOUT 80
    RUN_SERIAL TRUE
    LABELS "app-instance-actions;production-linked"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
)

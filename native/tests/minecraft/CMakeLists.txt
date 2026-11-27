set(minecraft_test_targets nickel_minecraft_policy_tests nickel_minecraft_planning_tests nickel_minecraft_lifecycle_tests)
add_executable(nickel_minecraft_policy_tests policy_tests.cpp)
add_executable(nickel_minecraft_planning_tests planning_tests.cpp)
add_executable(nickel_minecraft_lifecycle_tests lifecycle_tests.cpp)
add_executable(nickel_minecraft_mock_java mock_java.cpp)
if(NICKEL_MINECRAFT_NATIVE_SERVICES OR (TARGET nickel_foundation AND TARGET nickel_transport))
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/integration_tests.cpp")
        add_executable(nickel_minecraft_integration_tests integration_tests.cpp)
        add_executable(nickel_minecraft_handoff_tests handoff_tests.cpp allocation_fault.cpp)
        add_executable(nickel_minecraft_shared_http_tests shared_http_tests.cpp allocation_fault.cpp)
        list(APPEND minecraft_test_targets nickel_minecraft_integration_tests nickel_minecraft_handoff_tests nickel_minecraft_shared_http_tests)
    endif()
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/live_probe.cpp")
        add_executable(nickel_minecraft_live_probe live_probe.cpp live_environment.cpp)
        target_link_libraries(nickel_minecraft_live_probe PRIVATE user32 gdi32)
        add_executable(nickel_minecraft_live_environment_tests live_environment_tests.cpp live_environment.cpp)
        list(APPEND minecraft_test_targets nickel_minecraft_live_probe nickel_minecraft_live_environment_tests)
    endif()
endif()
foreach(target ${minecraft_test_targets} nickel_minecraft_mock_java)
    target_link_libraries(${target} PRIVATE nickel_minecraft)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../core/minecraft")
endforeach()
if(MSVC)
    foreach(target ${minecraft_test_targets} nickel_minecraft_mock_java)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()
add_test(NAME minecraft_policy_oracle COMMAND nickel_minecraft_policy_tests)
add_test(NAME minecraft_planning_oracle COMMAND nickel_minecraft_planning_tests)
add_test(NAME minecraft_lifecycle_synthetic COMMAND nickel_minecraft_lifecycle_tests)
set_tests_properties(minecraft_policy_oracle minecraft_planning_oracle minecraft_lifecycle_synthetic PROPERTIES LABELS "minecraft;synthetic")
set_tests_properties(minecraft_lifecycle_synthetic PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" TIMEOUT 30)
if(TARGET nickel_minecraft_integration_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach(case archives processes network installation runtime loaders processors paths)
        add_test(NAME minecraft_native_${case}_controlled
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/controlled_server.py"
                --mock "$<TARGET_FILE:nickel_minecraft_mock_java>"
                --test "$<TARGET_FILE:nickel_minecraft_integration_tests>" --case "${case}")
        set_tests_properties(minecraft_native_${case}_controlled PROPERTIES
            LABELS "minecraft;native-adapter;controlled-http;mock-java" TIMEOUT 150
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
    add_test(NAME minecraft_native_handoff_controlled
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/controlled_server.py"
            --mock "$<TARGET_FILE:nickel_minecraft_mock_java>"
            --test "$<TARGET_FILE:nickel_minecraft_handoff_tests>" --case handoff)
    set_tests_properties(minecraft_native_handoff_controlled PROPERTIES
        LABELS "minecraft;native-adapter;controlled-http;mock-java;allocation-fault" TIMEOUT 150
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(WIN32)
        add_test(NAME minecraft_native_held_pipes_controlled
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/controlled_server.py"
                --mock "$<TARGET_FILE:nickel_minecraft_mock_java>"
                --test "$<TARGET_FILE:nickel_minecraft_handoff_tests>" --case held-pipes)
        set_tests_properties(minecraft_native_held_pipes_controlled PROPERTIES
            LABELS "minecraft;native-adapter;controlled-http;mock-java;held-pipe;owned-job" TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
if(TARGET nickel_minecraft_shared_http_tests)
    add_test(NAME minecraft_shared_http_controlled
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/controlled_server.py"
            --mock "$<TARGET_FILE:nickel_minecraft_mock_java>"
            --test "$<TARGET_FILE:nickel_minecraft_shared_http_tests>" --case shared-http)
    set_tests_properties(minecraft_shared_http_controlled PROPERTIES
        LABELS "minecraft;native-adapter;controlled-http;shared-client;immutable-snapshot" TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(TARGET nickel_minecraft_live_environment_tests)
    add_test(NAME minecraft_live_environment_controlled COMMAND nickel_minecraft_live_environment_tests)
    set_tests_properties(minecraft_live_environment_controlled PROPERTIES
        LABELS "minecraft;native-adapter;controlled;physical-volume;child-environment" TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

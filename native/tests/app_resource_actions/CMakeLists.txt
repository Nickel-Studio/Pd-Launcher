if(NOT TARGET Nickel::AppServices)
    message(FATAL_ERROR "Resource action tests require the complete production AppServices target")
endif()

add_executable(nickel_app_resource_actions_tests resource_actions_tests.cpp)
target_compile_features(nickel_app_resource_actions_tests PRIVATE cxx_std_20)
target_link_libraries(nickel_app_resource_actions_tests PRIVATE Nickel::AppServices)
if(NICKEL_TEST_TRANSPORT_FAULTS)
    get_target_property(_resource_transport_definitions nickel_transport COMPILE_DEFINITIONS)
    get_target_property(_resource_transport_interface_definitions nickel_transport INTERFACE_COMPILE_DEFINITIONS)
    if(NOT "${_resource_transport_definitions};${_resource_transport_interface_definitions}" MATCHES "(^|;)NICKEL_TRANSPORT_TEST_HOOKS(=1)?(;|$)")
        message(FATAL_ERROR "Resource fault regressions require hooks in the actual linked production transport target")
    endif()
    target_compile_definitions(nickel_app_resource_actions_tests PRIVATE NICKEL_TRANSPORT_TEST_HOOKS=1)
else()
    message(STATUS "Resource update boundary regressions are BLOCKED: NICKEL_TEST_TRANSPORT_FAULTS is OFF")
endif()
if(MSVC)
    target_compile_options(nickel_app_resource_actions_tests PRIVATE /W4 /utf-8 /EHsc /permissive- /Zc:__cplusplus)
else()
    target_compile_options(nickel_app_resource_actions_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME nickel_app_resource_actions COMMAND nickel_app_resource_actions_tests)
set_tests_properties(nickel_app_resource_actions PROPERTIES
    TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RUN_SERIAL TRUE)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(NICKEL_RESOURCE_TEST_OPENSSL NAMES openssl REQUIRED)
add_test(NAME nickel_app_resource_network
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/run_network_tests.py"
            --openssl "${NICKEL_RESOURCE_TEST_OPENSSL}" "$<TARGET_FILE:nickel_app_resource_actions_tests>")
set_tests_properties(nickel_app_resource_network PROPERTIES
    TIMEOUT 160 ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RUN_SERIAL TRUE)

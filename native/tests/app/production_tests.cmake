if(NOT TARGET Nickel::AppServices)
    message(FATAL_ERROR "Production application tests require the real Nickel::AppServices target")
endif()
find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui Widgets Svg Test)
add_executable(nickel_application_production_tests
    "${CMAKE_CURRENT_LIST_DIR}/production_tests.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/production_lifecycle_tests.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/production_factory_tests.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/production_fixture.hpp"
)
target_link_libraries(nickel_application_production_tests PRIVATE Nickel::AppServices Qt6::Test)
target_compile_features(nickel_application_production_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nickel_application_production_tests PRIVATE /W4 /utf-8 /UNDEBUG)
else()
    target_compile_options(nickel_application_production_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
qt_import_plugins(nickel_application_production_tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
add_test(NAME nickel_application_production COMMAND nickel_application_production_tests)
set_tests_properties(nickel_application_production PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
add_test(NAME nickel_application_factory COMMAND nickel_application_production_tests --factory-only)
set_tests_properties(nickel_application_factory PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

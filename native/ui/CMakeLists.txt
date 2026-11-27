cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NickelUi LANGUAGES CXX)
    include(CTest)
    if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../tests/ui/ConfigureMsvc.cmake")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg)
add_library(nickel_ui STATIC
    BackgroundView.cpp BackgroundView.hpp Bindings.cpp Bindings.hpp Controls.cpp Controls.hpp DecodeImage.cpp DecodeImage.hpp
    Dialogs.cpp Dialogs.hpp DownloadBubble.cpp DownloadBubble.hpp DownloadPage.cpp
    GameForm.cpp GameForm.hpp HomePage.cpp Icons.cpp Icons.hpp LaunchError.cpp LaunchError.hpp
    ListModel.cpp ListModel.hpp MainWindow.cpp MainWindow.hpp NativeMarkdown.cpp NativeMarkdown.hpp
    MorePage.cpp Navigation.cpp Navigation.hpp PageShell.cpp PageShell.hpp Pages.hpp ProductIdentity.hpp
    ResourcesPage.cpp SafeDescription.cpp SafeDescription.hpp SettingsPage.cpp
    StartupGate.cpp StartupGate.hpp Theme.cpp Theme.hpp ThemeArtwork.cpp ThemeArtwork.hpp TitleBar.cpp TitleBar.hpp
    UiServices.hpp VersionSettingsPage.cpp VersionsPage.cpp
)
add_library(Nickel::Ui ALIAS nickel_ui)
set_target_properties(nickel_ui PROPERTIES AUTOMOC ON)
target_compile_features(nickel_ui PUBLIC cxx_std_20)
target_include_directories(nickel_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(NICKEL_UI_DOMAIN_INCLUDE_ROOT "" CACHE PATH "Canonical native domain header root, supplied by integration")
if(NICKEL_UI_DOMAIN_INCLUDE_ROOT)
    target_include_directories(nickel_ui PUBLIC "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}")
endif()
target_link_libraries(nickel_ui PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets PRIVATE Qt6::Svg)
if(TARGET nickel_common)
    target_link_libraries(nickel_ui PUBLIC nickel_common)
else()
    find_package(nlohmann_json 3.11 REQUIRED CONFIG)
    target_link_libraries(nickel_ui PUBLIC nlohmann_json::nlohmann_json)
endif()
get_filename_component(_nickel_ui_tests_root "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ui" ABSOLUTE)
if(TARGET Nickel::CommunityQt)
    target_link_libraries(nickel_ui PRIVATE Nickel::CommunityQt)
elseif(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR CMAKE_SOURCE_DIR STREQUAL _nickel_ui_tests_root)
    include("${_nickel_ui_tests_root}/CanonicalDomains.cmake")
    target_link_libraries(nickel_ui PRIVATE nickel_ui_canonical_documents)
else()
    message(FATAL_ERROR "The production build must provide Nickel::CommunityQt before Nickel::Ui")
endif()
if(MSVC)
    target_compile_options(nickel_ui PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(nickel_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../tests/ui" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()

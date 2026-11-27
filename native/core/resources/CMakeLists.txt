if(TARGET nickel_community)
    return()
endif()
foreach(_dependency nickel_common nickel_transport nickel_instances)
    if(NOT TARGET "${_dependency}")
        message(FATAL_ERROR "The root build must provide canonical ${_dependency} before community")
    endif()
endforeach()
if(NOT TARGET gumbo::gumbo)
    find_package(gumbo CONFIG REQUIRED)
endif()
if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus CONFIG REQUIRED)
endif()

add_library(nickel_community STATIC
    types.cpp
    network.cpp
    community.cpp
    mods.cpp
    mods_task.cpp
    utility.cpp
    file_identity.cpp
    adapters.cpp
    export.cpp
    ../documents/body.cpp
)
add_library(Nickel::Community ALIAS nickel_community)
target_compile_features(nickel_community PUBLIC cxx_std_20)
target_include_directories(nickel_community PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nickel_community PUBLIC nickel_common nickel_transport nickel_instances
    PRIVATE gumbo::gumbo tomlplusplus::tomlplusplus)

option(NICKEL_COMMUNITY_QT_ADAPTER "Build the controlled Qt document adapter" ON)
if(NICKEL_COMMUNITY_QT_ADAPTER)
    find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui)
    add_library(nickel_community_qt STATIC ../documents/qt_body.cpp)
    add_library(Nickel::CommunityQt ALIAS nickel_community_qt)
    target_compile_features(nickel_community_qt PUBLIC cxx_std_20)
    target_link_libraries(nickel_community_qt PUBLIC nickel_community Qt6::Core Qt6::Gui)
endif()

foreach(_target nickel_community nickel_community_qt)
    if(NOT TARGET "${_target}")
        continue()
    endif()
    if(MSVC)
        target_compile_options("${_target}" PRIVATE /W4 /permissive- /utf-8 /EHsc)
        if(_target STREQUAL "nickel_community")
            # Gumbo C callbacks may throw; keep C++ unwinding without catching SEH.
            set_property(SOURCE ../documents/body.cpp APPEND PROPERTY COMPILE_OPTIONS /EHsc-)
        endif()
        set_property(TARGET "${_target}" PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options("${_target}" PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

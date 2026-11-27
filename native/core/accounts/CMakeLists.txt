cmake_minimum_required(VERSION 3.24)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 CONFIG REQUIRED)
endif()

if(NOT TARGET nickel_common)
    message(FATAL_ERROR "nickel_identity requires the canonical nickel_common target")
endif()

if(NOT TARGET ZLIB::ZLIB)
    set(ZLIB_USE_STATIC_LIBS ON)
    find_package(ZLIB REQUIRED)
endif()

add_library(nickel_identity STATIC
    accounts.cpp
    auth.cpp
    error_codes.cpp
    http_port.cpp
    detail/util.cpp
    detail/protocol.cpp
    ../media/geometry.cpp
    ../media/images.cpp
    ../media/skin_png.cpp
    ../media/offline_skin.cpp
)
target_compile_features(nickel_identity PUBLIC cxx_std_20)
target_include_directories(nickel_identity PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(nickel_identity PUBLIC nickel_common PRIVATE ZLIB::ZLIB)
set_target_properties(nickel_identity PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(nickel_identity PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_libraries(nickel_identity PRIVATE bcrypt)
else()
    target_compile_options(nickel_identity PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(NICKEL_IDENTITY_CONTROLLED_ONLY "Allow identity core tests without production owner targets" OFF)
if(TARGET nickel_foundation)
    target_sources(nickel_identity PRIVATE foundation_adapter.cpp)
    target_link_libraries(nickel_identity PUBLIC nickel_foundation)
elseif(NOT NICKEL_IDENTITY_CONTROLLED_ONLY)
    message(FATAL_ERROR "nickel_identity requires the actual nickel_foundation target")
endif()
if(TARGET nickel_transport)
    target_sources(nickel_identity PRIVATE transport_adapter.cpp ../media/offline_skin_native.cpp)
    target_link_libraries(nickel_identity PUBLIC nickel_transport)
elseif(NOT NICKEL_IDENTITY_CONTROLLED_ONLY)
    message(FATAL_ERROR "nickel_identity requires the actual nickel_transport target")
endif()

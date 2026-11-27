if(TARGET nickel_transport)
    return()
endif()
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(nickel_transport STATIC
    http.cpp
    url_policy.cpp
    download.cpp
    hash.cpp
    budget.cpp
    control.cpp
)
target_compile_features(nickel_transport PUBLIC cxx_std_20)
target_include_directories(nickel_transport PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(nickel_transport PUBLIC CURL::libcurl nlohmann_json::nlohmann_json
    PRIVATE LibArchive::LibArchive ZLIB::ZLIB BZip2::BZip2 LibLZMA::LibLZMA zstd::libzstd_static)
if(NOT TARGET nickel_common)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../common" "${CMAKE_CURRENT_BINARY_DIR}/common")
endif()
target_link_libraries(nickel_transport PUBLIC nickel_common)
if(WIN32)
    if(NOT TARGET nickel_foundation)
        message(FATAL_ERROR "Windows transport requires the canonical nickel_foundation target")
    endif()
    target_link_libraries(nickel_transport PUBLIC nickel_foundation)
endif()
if(MSVC)
    target_compile_options(nickel_transport PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    set_property(TARGET nickel_transport PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(nickel_transport PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 CURL_STATICLIB LIBARCHIVE_STATIC)
    target_link_libraries(nickel_transport PRIVATE bcrypt ws2_32 xmllite)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(nickel_transport PRIVATE OpenSSL::Crypto)
endif()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tasks" "${CMAKE_CURRENT_BINARY_DIR}/tasks")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../archive" "${CMAKE_CURRENT_BINARY_DIR}/archive")

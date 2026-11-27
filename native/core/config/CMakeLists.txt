add_library(nickel_foundation STATIC
    atomic_file.cpp
    crypto.cpp
    detail.cpp
    path_guard.cpp
    paths.cpp
    repository.cpp
    vault.cpp
)
get_filename_component(NICKEL_FOUNDATION_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
target_include_directories(nickel_foundation PUBLIC "${NICKEL_FOUNDATION_NATIVE_ROOT}")
target_compile_features(nickel_foundation PUBLIC cxx_std_20)
target_link_libraries(nickel_foundation PUBLIC nickel_common)
find_package(Threads REQUIRED)
target_link_libraries(nickel_foundation PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(nickel_foundation PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(nickel_foundation PRIVATE bcrypt advapi32 shell32 ole32 user32 uuid)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(nickel_foundation PRIVATE OpenSSL::Crypto)
endif()
if(MSVC)
    target_compile_options(nickel_foundation PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(nickel_foundation PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_subdirectory("${NICKEL_FOUNDATION_NATIVE_ROOT}/platform" "${CMAKE_CURRENT_BINARY_DIR}/platform")

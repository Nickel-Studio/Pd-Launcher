include_guard(GLOBAL)

# EXACT pins are the versions produced by scripts/native/bootstrap-deps.py; change them only
# together with that bootstrap recipe so the static archive stack stays reproducible.
set(ZLIB_USE_STATIC_LIBS ON)
set(CURL_USE_STATIC_LIBS ON)
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(nlohmann_json 3.11.3 EXACT CONFIG REQUIRED)
find_package(ZLIB 1.3.1 EXACT REQUIRED)
find_package(CURL 8.23.0 EXACT CONFIG REQUIRED)
find_package(BZip2 1.0.8 EXACT REQUIRED)
find_package(LibLZMA 5.8.1 EXACT REQUIRED)
find_package(zstd 1.5.7 EXACT CONFIG REQUIRED)
find_package(LibArchive 3.8.9 EXACT REQUIRED)
find_package(gumbo 0.10.1 EXACT CONFIG REQUIRED)
find_package(tomlplusplus 3.4.0 EXACT CONFIG REQUIRED)
find_package(yaml 0.2.5 EXACT CONFIG REQUIRED)
find_package(Threads REQUIRED)
if(NOT TARGET nickel_gumbo)
    add_library(nickel_gumbo INTERFACE)
    target_link_libraries(nickel_gumbo INTERFACE gumbo::gumbo)
endif()
if(WIN32)
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC)
    set_property(TARGET LibLZMA::LibLZMA APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS LZMA_API_STATIC)
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB;BZip2::BZip2;LibLZMA::LibLZMA;zstd::libzstd_static;bcrypt;xmllite;uuid")
    include("${CMAKE_CURRENT_LIST_DIR}/NickelStaticDependencies.cmake")
    nickel_require_windows_static_zlib(ZLIB::ZLIB)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)
    set_property(TARGET LibArchive::LibArchive APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB;BZip2::BZip2;LibLZMA::LibLZMA;zstd::libzstd_static")
endif()

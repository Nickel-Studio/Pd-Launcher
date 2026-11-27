# Standalone UI tests link the real canonical document implementation, not a parser fixture.
if(TARGET nickel_ui_canonical_documents)
    return()
endif()
if(NOT NICKEL_UI_DOMAIN_INCLUDE_ROOT)
    message(FATAL_ERROR "Standalone UI tests require NICKEL_UI_DOMAIN_INCLUDE_ROOT with canonical sources")
endif()
find_package(gumbo CONFIG REQUIRED)
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)
set(_ui_document_sources
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/documents/qt_body.cpp"
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/documents/body.cpp"
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/resources/types.cpp"
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/resources/network.cpp"
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/network/hash.cpp"
    "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/tasks/secure_fs.cpp"
)
foreach(_source IN LISTS _ui_document_sources)
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR "Canonical UI test source is missing: ${_source}")
    endif()
endforeach()
add_library(nickel_ui_canonical_documents STATIC ${_ui_document_sources})
target_compile_features(nickel_ui_canonical_documents PUBLIC cxx_std_20)
get_filename_component(_ui_native_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
target_include_directories(nickel_ui_canonical_documents PUBLIC "${_ui_native_root}" "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}")
target_link_libraries(nickel_ui_canonical_documents PUBLIC Qt6::Core Qt6::Gui nlohmann_json::nlohmann_json
    PRIVATE gumbo::gumbo ZLIB::ZLIB)
if(MSVC)
    target_compile_options(nickel_ui_canonical_documents PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    set_property(SOURCE "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/core/documents/body.cpp" APPEND PROPERTY COMPILE_OPTIONS /EHsc-)
endif()
if(WIN32)
    target_link_libraries(nickel_ui_canonical_documents PRIVATE bcrypt)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(nickel_ui_canonical_documents PRIVATE OpenSSL::Crypto)
endif()

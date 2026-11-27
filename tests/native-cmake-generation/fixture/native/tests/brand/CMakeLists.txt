set(_resource_source "${CMAKE_CURRENT_LIST_DIR}/../../assets/brand/functional-resources.qrc")
file(READ "${_resource_source}" _resource_contents)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_resource_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc" "${_resource_contents}")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
        "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
    VERBATIM
)
add_custom_target(pd_brand_fixture ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/brand-resource-copy.qrc"
)

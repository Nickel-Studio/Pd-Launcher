set(_nickel_asset_source "${CMAKE_SOURCE_DIR}/native/assets/brand")
include("${CMAKE_CURRENT_LIST_DIR}/app-resource-input.fragment.cmake")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
        "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc"
    VERBATIM
)
add_custom_target(pd_app_resource_copy ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/app-resource-copy.qrc"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app-configure-inputs.txt"
    "${_nickel_asset_source}/functional-resources.qrc\n"
    "${CMAKE_CURRENT_BINARY_DIR}/functional-resources.qrc\n"
)

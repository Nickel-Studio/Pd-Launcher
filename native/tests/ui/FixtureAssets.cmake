# Preserve the approved QRC aliases and original bytes; only paths change in the test build tree.
get_filename_component(_ui_fixture_worktree "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_ui_fixture_brand "${NICKEL_UI_DOMAIN_INCLUDE_ROOT}/assets/brand")
if(NOT EXISTS "${_ui_fixture_brand}/brand.qrc" OR NOT EXISTS "${_ui_fixture_brand}/functional-resources.qrc")
    message(FATAL_ERROR "UI fixtures require the approved brand and functional QRC files")
endif()
file(READ "${_ui_fixture_brand}/functional-resources.qrc" _ui_functional_qrc)
string(REPLACE "../../../src/" "${_ui_fixture_worktree}/src/" _ui_functional_qrc "${_ui_functional_qrc}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fixture-functional-resources.qrc" "${_ui_functional_qrc}")
qt_add_resources(_ui_fixture_resource_sources
    "${_ui_fixture_brand}/brand.qrc"
    "${CMAKE_CURRENT_BINARY_DIR}/fixture-functional-resources.qrc"
)
add_library(nickel_ui_fixture_assets OBJECT ${_ui_fixture_resource_sources})
target_link_libraries(nickel_ui_fixture_assets PRIVATE Qt6::Core)

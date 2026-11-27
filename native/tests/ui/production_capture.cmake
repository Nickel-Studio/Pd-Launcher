# Included by the root build owner; this observer never links launcher UI or services.
if(NOT WIN32 OR TARGET pd_production_capture)
    return()
endif()
find_package(Qt6 6.8.3 EXACT CONFIG REQUIRED COMPONENTS Core Gui)
add_executable(pd_production_capture "${CMAKE_CURRENT_LIST_DIR}/production_capture_win.cpp")
set_target_properties(pd_production_capture PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    WIN32_EXECUTABLE OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_features(pd_production_capture PRIVATE cxx_std_20)
target_compile_definitions(pd_production_capture PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 QT_NO_KEYWORDS
)
target_link_libraries(pd_production_capture PRIVATE
    Qt6::Core Qt6::Gui windowsapp d3d11 dxgi dwmapi shcore
)
if(MSVC)
    target_compile_options(pd_production_capture PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj)
endif()

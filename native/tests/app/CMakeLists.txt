cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nickel_application_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    enable_testing()
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

add_executable(nickel_request_registry_tests
    request_registry_tests.cpp
    ../../app/service/request_registry.cpp
)
target_include_directories(nickel_request_registry_tests PRIVATE ../..)
target_link_libraries(nickel_request_registry_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(nickel_request_registry_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nickel_request_registry_tests PRIVATE /W4 /utf-8 /UNDEBUG)
else()
    target_compile_options(nickel_request_registry_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME nickel_request_registry COMMAND nickel_request_registry_tests)

add_executable(nickel_mutation_gate_tests
    mutation_gate_tests.cpp
    ../../app/service/mutation_gate.cpp
)
target_include_directories(nickel_mutation_gate_tests PRIVATE ../..)
target_link_libraries(nickel_mutation_gate_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(nickel_mutation_gate_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nickel_mutation_gate_tests PRIVATE /W4 /utf-8 /UNDEBUG)
else()
    target_compile_options(nickel_mutation_gate_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME nickel_mutation_gate COMMAND nickel_mutation_gate_tests)

find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Core Gui)
if(NOT DEFINED ZLIB_USE_STATIC_LIBS)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
add_executable(nickel_public_dto_tests
    public_dto_tests.cpp
    ../../app/service/public_dto.cpp
)
target_include_directories(nickel_public_dto_tests PRIVATE ../..)
target_link_libraries(nickel_public_dto_tests PRIVATE nlohmann_json::nlohmann_json Qt6::Core Qt6::Gui ZLIB::ZLIB)
target_compile_features(nickel_public_dto_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nickel_public_dto_tests PRIVATE /W4 /utf-8 /UNDEBUG)
else()
    target_compile_options(nickel_public_dto_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME nickel_public_dto COMMAND nickel_public_dto_tests)

get_filename_component(NICKEL_APP_WORKTREE "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(NICKEL_APP_WORKER_QUEUE_SOURCE "${NICKEL_APP_WORKTREE}/native/app/WorkerQueue.cpp"
    CACHE FILEPATH "Canonical builder-owned WorkerQueue implementation")
if(NOT EXISTS "${NICKEL_APP_WORKER_QUEUE_SOURCE}")
    message(FATAL_ERROR "Supply NICKEL_APP_WORKER_QUEUE_SOURCE with the actual canonical WorkerQueue source")
endif()
get_filename_component(NICKEL_APP_WORKER_QUEUE_DIR "${NICKEL_APP_WORKER_QUEUE_SOURCE}" DIRECTORY)
get_filename_component(NICKEL_APP_WORKER_QUEUE_INCLUDE_ROOT "${NICKEL_APP_WORKER_QUEUE_DIR}/.." ABSOLUTE)
add_executable(nickel_operation_queue_tests
    operation_queue_tests.cpp
    "${NICKEL_APP_WORKER_QUEUE_SOURCE}"
)
target_include_directories(nickel_operation_queue_tests BEFORE PRIVATE "${NICKEL_APP_WORKER_QUEUE_INCLUDE_ROOT}" ../..)
target_link_libraries(nickel_operation_queue_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(nickel_operation_queue_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nickel_operation_queue_tests PRIVATE /W4 /utf-8 /UNDEBUG)
else()
    target_compile_options(nickel_operation_queue_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
endif()
add_test(NAME nickel_operation_queue COMMAND nickel_operation_queue_tests)

include(FetchContent)

# Pin GoogleTest so local and CI builds use the same dependency revision.
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(aegis_fault_process fixtures/fault_process.cpp)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aegis_fault_process PRIVATE -Wall -Wextra -Wpedantic)
endif ()

add_executable(aegis_agent_core_tests
        unit/model_validation_test.cpp
        unit/health_checker_test.cpp
        unit/process_supervisor_state_test.cpp
        unit/recovery_manager_test.cpp
        unit/agent_api_status_test.cpp
        support/test_builders.h
)

target_include_directories(aegis_agent_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegis_agent_core_tests PRIVATE aegis_agent_core GTest::gtest_main)

target_compile_definitions(aegis_agent_core_tests PRIVATE
        AEGIS_TEST_FAULT_PROCESS_PATH="$<TARGET_FILE:aegis_fault_process>"
        AEGIS_TEST_DEMO_SERVICE_PATH="$<TARGET_FILE:demo_service>"
)

add_dependencies(aegis_agent_core_tests aegis_fault_process demo_service)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aegis_agent_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif ()

include(GoogleTest)
gtest_discover_tests(
        aegis_agent_core_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
        LABELS unit
        TIMEOUT 180
)

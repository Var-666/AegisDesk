add_executable(demo_service main.cpp)

target_compile_definitions(demo_service PRIVATE
        AEGISDESK_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(demo_service PRIVATE -Wall -Wextra -Wpedantic)
endif ()
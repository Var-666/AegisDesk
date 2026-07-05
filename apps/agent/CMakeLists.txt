find_package(Boost 1.74 REQUIRED)
find_package(Threads REQUIRED)

add_executable(agent main.cpp
        src/process_supervisor.cpp
        src/agent_api.cpp
        src/http_json.cpp
        src/http_server.cpp
        src/log_reader.cpp
        include/agent/process_supervisor.h
        include/agent/log_reader.h
        include/agent/http_json.h
)

target_include_directories(agent PRIVATE include)
target_include_directories(agent SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(agent PRIVATE Threads::Threads Boost::headers)

# Boost.System 使用头文件模式，避免额外链接库配置问题。
target_compile_definitions(agent PRIVATE
        BOOST_ERROR_CODE_HEADER_ONLY
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(agent PRIVATE -Wall -Wextra -Wpedantic)
endif ()
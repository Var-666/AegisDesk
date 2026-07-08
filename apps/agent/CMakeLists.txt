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
        include/agent/service_definition.h
        src/service_registry.cpp
        include/agent/service_registry.h
        include/agent/service_metrics.h
        src/metrics_sampler.cpp
        include/agent/metrics_sampler.h
        src/metrics_collector.cpp
        include/agent/metrics_collector.h
        include/agent/platform_metrics_reader.h
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(agent PRIVATE src/linux_metrics_reader.cpp)
elseif (APPLE)
    target_sources(agent PRIVATE src/macos_metrics_reader.cpp)
    find_library(AEGIS_LIBPROC_LIBRARY NAMES proc)

    if (NOT AEGIS_LIBPROC_LIBRARY)
        message(FATAL_ERROR "macOS libproc library was not found")
    endif ()

    target_link_libraries(agent PRIVATE ${AEGIS_LIBPROC_LIBRARY})
else ()
    target_sources(agent PRIVATE src/unsupported_metrics_reader.cpp)
endif ()

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
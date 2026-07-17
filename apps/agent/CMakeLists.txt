find_package(Boost 1.74 REQUIRED)
find_package(Threads REQUIRED)

add_library(aegis_agent_core STATIC
        src/api/agent_api.cpp
        src/api/http_json.cpp
        include/agent/api/agent_api.h
        include/agent/api/http_json.h

        src/service/log_reader.cpp
        src/service/process_supervisor.cpp
        src/service/service_registry.cpp
        include/agent/service/log_reader.h
        include/agent/service/desired_state.h
        include/agent/service/process_supervisor.h
        include/agent/service/service_definition.h
        include/agent/service/service_id.h
        include/agent/service/service_registry.h

        src/metrics/metrics_collector.cpp
        src/metrics/metrics_sampler.cpp
        include/agent/metrics/metrics_collector.h
        include/agent/metrics/metrics_sampler.h
        include/agent/metrics/platform_metrics_reader.h
        include/agent/metrics/service_metrics.h

        src/health/health_checker.cpp
        src/health/health_monitor.cpp
        include/agent/health/health_checker.h
        include/agent/health/health_monitor.h
        include/agent/health/health_status.h

        src/alerts/alert_evaluator.cpp
        src/alerts/alert_manager.cpp
        include/agent/alerts/alert_evaluator.h
        include/agent/alerts/alert_event.h
        include/agent/alerts/alert_manager.h
        include/agent/alerts/alert_rule.h

        src/recovery/recovery_manager.cpp
        include/agent/recovery/recovery_manager.h
        include/agent/recovery/recovery_policy.h
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(aegis_agent_core PRIVATE src/metrics/linux_metrics_reader.cpp)
elseif (APPLE)
    target_sources(aegis_agent_core PRIVATE src/metrics/macos_metrics_reader.cpp)
    find_library(AEGIS_LIBPROC_LIBRARY NAMES proc)

    if (NOT AEGIS_LIBPROC_LIBRARY)
        message(FATAL_ERROR "macOS libproc library was not found")
    endif ()

    target_link_libraries(aegis_agent_core PRIVATE ${AEGIS_LIBPROC_LIBRARY})
else ()
    target_sources(aegis_agent_core PRIVATE src/metrics/unsupported_metrics_reader.cpp)
endif ()

target_include_directories(aegis_agent_core
        PUBLIC include
        SYSTEM PRIVATE ${Boost_INCLUDE_DIRS}
)

target_link_libraries(aegis_agent_core PUBLIC Threads::Threads Boost::headers)

# Boost.System 使用头文件模式，避免额外链接库配置问题。
target_compile_definitions(aegis_agent_core PUBLIC
        BOOST_ERROR_CODE_HEADER_ONLY
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aegis_agent_core PRIVATE -Wall -Wextra -Wpedantic)
endif ()

add_library(aegis_agent_http STATIC
        src/api/http_server.cpp
        include/agent/api/http_server.h
)

target_include_directories(aegis_agent_http PUBLIC include)
target_link_libraries(aegis_agent_http PUBLIC aegis_agent_core)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aegis_agent_http PRIVATE -Wall -Wextra -Wpedantic)
endif ()

add_executable(agent main.cpp)

target_link_libraries(agent PRIVATE aegis_agent_http)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(agent PRIVATE -Wall -Wextra -Wpedantic)
endif ()

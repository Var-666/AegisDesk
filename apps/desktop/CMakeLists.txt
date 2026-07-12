find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Charts)

add_executable(desktop
        src/main.cpp

        src/api/agent_client.cpp
        include/desktop/api/agent_client.h

        src/widgets/main_window.cpp
        src/widgets/metric_chart_widget.cpp
        src/widgets/metrics_history_panel.cpp
        src/widgets/service_health_panel.cpp
        include/desktop/widgets/main_window.h
        include/desktop/widgets/metric_chart_widget.h
        include/desktop/widgets/metrics_history_panel.h
        include/desktop/widgets/service_health_panel.h

        src/common/ui_helpers.cpp
        include/desktop/common/ui_helpers.h
)

target_include_directories(desktop PRIVATE include)

target_link_libraries(desktop PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Charts)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(desktop PRIVATE -Wall -Wextra -Wpedantic)
endif ()

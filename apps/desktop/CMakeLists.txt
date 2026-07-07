find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Charts)

add_executable(desktop src/main.cpp
        src/main_window.cpp
        include/desktop/main_window.h
        src/agent_client.cpp
        include/desktop/agent_client.h
        src/metric_chart_widget.cpp
        include/desktop/metric_chart_widget.h
        src/metrics_history_panel.cpp
        include/desktop/metrics_history_panel.h
)


target_include_directories(desktop PRIVATE include)

target_link_libraries(desktop PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Charts)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(desktop PRIVATE -Wall -Wextra -Wpedantic)
endif ()
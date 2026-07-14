cmake_minimum_required(VERSION 3.30)
project(AegisDesk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_ASM_EXTENSIONS OFF)

include(CTest)

add_subdirectory(apps/demo_service)

set(Boost_ROOT "/opt/homebrew/opt/boost")
add_subdirectory(apps/agent)

add_subdirectory(apps/desktop)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()

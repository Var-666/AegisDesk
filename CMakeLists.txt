cmake_minimum_required(VERSION 3.25)
project(AegisDesk LANGUAGES C CXX)

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AEGIS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AEGIS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(AEGIS_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if (AEGIS_ENABLE_ASAN OR AEGIS_ENABLE_UBSAN OR AEGIS_ENABLE_TSAN)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "AegisDesk sanitizer builds require Clang or GCC")
    endif ()

    if (AEGIS_ENABLE_TSAN AND (AEGIS_ENABLE_ASAN OR AEGIS_ENABLE_UBSAN))
        message(FATAL_ERROR "ThreadSanitizer must use a separate build from ASan/UBSan")
    endif ()

    set(AEGIS_SANITIZER_FLAGS -fno-omit-frame-pointer)

    if (AEGIS_ENABLE_ASAN)
        list(APPEND AEGIS_SANITIZER_FLAGS -fsanitize=address)
    endif ()

    if (AEGIS_ENABLE_UBSAN)
        list(APPEND AEGIS_SANITIZER_FLAGS -fsanitize=undefined)
    endif ()

    if (AEGIS_ENABLE_TSAN)
        list(APPEND AEGIS_SANITIZER_FLAGS -fsanitize=thread)
    endif ()

    add_compile_options(${AEGIS_SANITIZER_FLAGS})
    add_link_options(${AEGIS_SANITIZER_FLAGS})
endif ()

include(CTest)

add_subdirectory(apps/demo_service)

if (APPLE AND NOT DEFINED Boost_ROOT AND EXISTS "/opt/homebrew/opt/boost")
    set(Boost_ROOT "/opt/homebrew/opt/boost")
endif ()
add_subdirectory(apps/agent)

add_subdirectory(apps/desktop)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()

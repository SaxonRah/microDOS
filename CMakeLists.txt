cmake_minimum_required(VERSION 3.16)
project(microdos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(MD_BUILD_HOST "Build the desktop validation runner" ON)
option(MD_BUILD_TESTS "Build runtime tests" ON)
option(MD_FORCE_SWITCH_DISPATCH "Disable GNU/Clang direct-threaded interpreter dispatch" OFF)

add_library(microdos_runtime STATIC
    src/runtime/runtime.c
    src/runtime/x86_interp.c
)
add_library(microdos::runtime ALIAS microdos_runtime)

target_include_directories(microdos_runtime
    PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/runtime"
)

if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MD_FORCE_SWITCH_DISPATCH)
    target_compile_definitions(microdos_runtime PRIVATE MD_THREADED_DISPATCH=1)
endif()

if(MSVC)
    target_compile_options(microdos_runtime PRIVATE /W4 /permissive-)
    target_compile_options(microdos_runtime PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(microdos_runtime PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(microdos_runtime PRIVATE $<$<CONFIG:Release>:-O3>)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(microdos_runtime PRIVATE -Wno-pedantic)
    endif()
endif()

if(MD_BUILD_HOST)
    add_executable(microdos_host
        src/host/main.c
        src/host/host_dos.c
        src/generated/hello_recomp.c
    )
    target_link_libraries(microdos_host PRIVATE microdos::runtime)
    target_include_directories(microdos_host PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host")

    add_executable(microdos_bench src/host/bench.c)
    target_link_libraries(microdos_bench PRIVATE microdos::runtime)
endif()

if(MD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(microdos_tests
        tests/test_runtime.c
        src/host/host_dos.c
        src/generated/hello_recomp.c
    )
    target_link_libraries(microdos_tests PRIVATE microdos::runtime)
    target_include_directories(microdos_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host")
    add_test(NAME microdos_runtime COMMAND microdos_tests)
endif()

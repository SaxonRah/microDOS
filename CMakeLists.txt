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
    src/runtime/x86_block_cache.c
)
add_library(microdos::runtime ALIAS microdos_runtime)
target_include_directories(microdos_runtime PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/runtime")
if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MD_FORCE_SWITCH_DISPATCH)
    target_compile_definitions(microdos_runtime PRIVATE MD_THREADED_DISPATCH=1)
endif()

add_executable(dosrecomp tools/dosrecomp/main.c)

if(MSVC)
    target_compile_options(microdos_runtime PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_options(dosrecomp PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_definitions(dosrecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(microdos_runtime PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    target_compile_options(dosrecomp PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(microdos_runtime PRIVATE -Wno-pedantic)
    endif()
endif()

set(MD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MD_GENERATED_DIR}")

function(md_recompile_com name symbol input code_end)
    set(out_c "${MD_GENERATED_DIR}/${name}_recomp.c")
    set(out_h "${MD_GENERATED_DIR}/${name}_recomp.h")
    add_custom_command(
        OUTPUT "${out_c}" "${out_h}"
        COMMAND $<TARGET_FILE:dosrecomp>
            --input "${CMAKE_CURRENT_LIST_DIR}/${input}"
            --output-c "${out_c}"
            --output-h "${out_h}"
            --symbol "${symbol}"
            --code-end "${code_end}"
        DEPENDS dosrecomp "${CMAKE_CURRENT_LIST_DIR}/${input}"
        COMMENT "dosrecomp ${name}.com"
        VERBATIM
    )
    set(MD_RECOMP_${name}_C "${out_c}" PARENT_SCOPE)
    set(MD_RECOMP_${name}_H "${out_h}" PARENT_SCOPE)
endfunction()

md_recompile_com(hello md_recomp_hello tests/programs/hello.com 0x10c)
md_recompile_com(loop md_recomp_loop tests/programs/loop.com 0x107)
md_recompile_com(hybrid md_recomp_hybrid tests/programs/hybrid.com 0x109)

if(MD_BUILD_HOST)
    add_executable(microdos_host src/host/main.c src/host/host_dos.c "${MD_RECOMP_hello_C}" "${MD_RECOMP_hello_H}")
    target_link_libraries(microdos_host PRIVATE microdos::runtime)
    target_include_directories(microdos_host PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")

    add_executable(microdos_bench src/host/bench.c "${MD_RECOMP_loop_C}" "${MD_RECOMP_loop_H}")
    target_link_libraries(microdos_bench PRIVATE microdos::runtime)
    target_include_directories(microdos_bench PRIVATE "${MD_GENERATED_DIR}")
endif()

if(MD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(microdos_tests
        tests/test_runtime.c src/host/host_dos.c
        "${MD_RECOMP_hello_C}" "${MD_RECOMP_hello_H}"
        "${MD_RECOMP_loop_C}" "${MD_RECOMP_loop_H}"
        "${MD_RECOMP_hybrid_C}" "${MD_RECOMP_hybrid_H}")
    target_link_libraries(microdos_tests PRIVATE microdos::runtime)
    target_include_directories(microdos_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")
    add_test(NAME microdos_runtime COMMAND microdos_tests)
endif()

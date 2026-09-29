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

add_library(microdos_decode STATIC
    src/decode/x86_decode.c
)
add_library(microdos::decode ALIAS microdos_decode)
target_include_directories(microdos_decode PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

add_executable(dosrecomp tools/dosrecomp/main.c)
target_link_libraries(dosrecomp PRIVATE microdos::decode)
add_executable(dosprobe tools/dosprobe/main.c)
add_executable(mkfat12 tools/mkfat12/main.c)
target_link_libraries(dosprobe PRIVATE microdos::decode)
target_include_directories(dosprobe PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")

if(MSVC)
    target_compile_options(microdos_runtime PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_options(dosrecomp PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_options(dosprobe PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_options(microdos_decode PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_options(mkfat12 PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    target_compile_definitions(dosrecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(dosprobe PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(mkfat12 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(microdos_runtime PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    target_compile_options(dosrecomp PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    target_compile_options(dosprobe PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    target_compile_options(microdos_decode PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    target_compile_options(mkfat12 PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(microdos_runtime PRIVATE -Wno-pedantic)
    endif()
endif()

set(MD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MD_GENERATED_DIR}")

# Extra arguments after code_end are passed to dosrecomp unchanged
# (--entry N, --entries FILE, --interp-at N, --name NAME).
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
            ${ARGN}
        DEPENDS dosrecomp "${CMAKE_CURRENT_LIST_DIR}/${input}" ${MD_RECOMP_EXTRA_DEPENDS}
        COMMENT "dosrecomp ${name}.com"
        VERBATIM
    )
    set(MD_RECOMP_${name}_C "${out_c}" PARENT_SCOPE)
    set(MD_RECOMP_${name}_H "${out_h}" PARENT_SCOPE)
endfunction()

md_recompile_com(hello md_recomp_hello tests/programs/hello.com 0x10c)
md_recompile_com(loop md_recomp_loop tests/programs/loop.com 0x107)
# 0105h MOV BX,AX is compiled by dosrecomp v2; pin it as an interpreter hole so
# this fixture keeps proving the AOT -> cached interpreter -> AOT handoff.
md_recompile_com(hybrid md_recomp_hybrid tests/programs/hybrid.com 0x109 --interp-at 0x105)
md_recompile_com(selfmod md_recomp_selfmod tests/programs/selfmod.com 0x107)

# M13: DOS2TEST.COM compiled natively and attached when DOS loads it at A>.
# Entry points come from the NASM listing (tests reached via CALL BX).
set(MD_RECOMP_EXTRA_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/tests/dos2/DOS2TEST.entries")
md_recompile_com(dos2test md_recomp_dos2test tests/dos2/DOS2TEST.COM 0x0B97
    --entries "${CMAKE_CURRENT_LIST_DIR}/tests/dos2/DOS2TEST.entries" --name DOS2TEST.COM)
unset(MD_RECOMP_EXTRA_DEPENDS)

if(MD_BUILD_HOST)
    add_executable(microdos_host src/host/main.c src/host/host_dos.c "${MD_RECOMP_hello_C}" "${MD_RECOMP_hello_H}")
    target_link_libraries(microdos_host PRIVATE microdos::runtime)
    target_include_directories(microdos_host PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")

    add_executable(microdos_msdos2 src/host/msdos2_main.c src/host/msdos2_boot.c
        "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_dos2test_H}")
    target_link_libraries(microdos_msdos2 PRIVATE microdos::runtime)
    target_include_directories(microdos_msdos2 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")
    if(MSVC)
        target_compile_definitions(microdos_msdos2 PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(microdos_msdos2 PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(microdos_msdos2 PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    endif()

    add_executable(microdos_bench src/host/bench.c "${MD_RECOMP_loop_C}" "${MD_RECOMP_loop_H}")
    target_link_libraries(microdos_bench PRIVATE microdos::runtime)
    target_include_directories(microdos_bench PRIVATE "${MD_GENERATED_DIR}")
endif()

if(MD_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(microdos_tests
        tests/test_runtime.c src/host/host_dos.c src/host/msdos2_boot.c
        "${MD_RECOMP_hello_C}" "${MD_RECOMP_hello_H}"
        "${MD_RECOMP_loop_C}" "${MD_RECOMP_loop_H}"
        "${MD_RECOMP_hybrid_C}" "${MD_RECOMP_hybrid_H}"
        "${MD_RECOMP_selfmod_C}" "${MD_RECOMP_selfmod_H}")
    target_link_libraries(microdos_tests PRIVATE microdos::runtime)
    target_include_directories(microdos_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")
    add_test(NAME microdos_runtime COMMAND microdos_tests)
    add_test(NAME dosprobe_selftest COMMAND dosprobe --selftest)
    add_test(NAME mkfat12_selftest COMMAND mkfat12 --selftest)
endif()

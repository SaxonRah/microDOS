cmake_minimum_required(VERSION 3.16)
project(microdos_conformance C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(MD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_library(md_conformance_runtime STATIC
    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/x86_block_cache.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/exec_router.c"
)
target_include_directories(md_conformance_runtime PUBLIC
    "${MD_ROOT}/include"
    PRIVATE "${MD_ROOT}/src/runtime"
)

if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_definitions(md_conformance_runtime PRIVATE MD_THREADED_DISPATCH=1)
endif()

add_library(md_conformance_decode STATIC
    "${MD_ROOT}/src/decode/x86_decode.c"
)
target_include_directories(md_conformance_decode PUBLIC "${MD_ROOT}/include")

add_executable(microdos_silicon_runner
    test_cpu_silicon.c
)
target_link_libraries(microdos_silicon_runner PRIVATE md_conformance_runtime)
target_include_directories(microdos_silicon_runner PRIVATE "${MD_ROOT}/include")

add_executable(microdos_random_router_diff
    test_random_router_diff.c
    "${MD_ROOT}/src/system/md_dos2_system.c"
    "${MD_ROOT}/src/host/msdos2_boot.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
)
target_link_libraries(microdos_random_router_diff PRIVATE
    md_conformance_runtime
    md_conformance_decode
)
target_include_directories(microdos_random_router_diff PRIVATE
    "${MD_ROOT}/include"
    "${MD_ROOT}/src/runtime"
    "${MD_ROOT}/src/host"
    "${MD_ROOT}/src/system"
)
target_compile_definitions(microdos_random_router_diff PRIVATE
    MICRODOS_ENABLE_JIT=1
    MD_EXEC_ENABLE_PROMOTION=1
    MD_EXEC_ENABLE_DIRECT=1
    MD_JIT_PROFILE=0
    MD_EXEC_PROFILE=0
    MD_JIT_LEGACY_HOTNESS=0
)

if(MSVC)
    foreach(t md_conformance_runtime md_conformance_decode
              microdos_silicon_runner microdos_random_router_diff)
        target_compile_options(${t} PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    endforeach()
    target_compile_definitions(microdos_silicon_runner PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(microdos_random_router_diff PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    foreach(t md_conformance_runtime md_conformance_decode
              microdos_silicon_runner microdos_random_router_diff)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    endforeach()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(md_conformance_runtime PRIVATE -Wno-pedantic)
    endif()
endif()

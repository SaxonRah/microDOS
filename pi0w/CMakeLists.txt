cmake_minimum_required(VERSION 3.16)
project(microdos_pi0w_baremetal C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(MD_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MICRODOS_HOST_BUILD "${MD_ROOT}/build-host" CACHE PATH "Desktop build directory")
set(MICRODOS_PI0W_KERNEL "${MD_ROOT}/third_party/msdos/v2.0/bin/MSDOS.SYS" CACHE FILEPATH "MSDOS.SYS")
set(MICRODOS_PI0W_DISK "${MD_ROOT}/build-pi0w/msdos2.img" CACHE FILEPATH "360K FAT12 image")
set(MD_GENERATED "${MICRODOS_HOST_BUILD}/generated")

foreach(req
        "${MD_GENERATED}/dos2test_recomp.c"
        "${MD_GENERATED}/msdos2_recomp.c"
        "${MD_GENERATED}/loop_recomp.c"
        "${MD_GENERATED}/memloop_recomp.c"
        "${MD_GENERATED}/checksum_recomp.c"
        "${MICRODOS_PI0W_KERNEL}"
        "${MICRODOS_PI0W_DISK}")
    if(NOT EXISTS "${req}")
        message(FATAL_ERROR "microDOS Pi Zero 2 W: missing ${req}")
    endif()
endforeach()

file(TO_CMAKE_PATH "${MICRODOS_PI0W_KERNEL}" MICRODOS_PI0W_KERNEL)
file(TO_CMAKE_PATH "${MICRODOS_PI0W_DISK}" MICRODOS_PI0W_DISK)
configure_file("${CMAKE_CURRENT_LIST_DIR}/md_pi0w_blobs.S.in"
               "${CMAKE_CURRENT_BINARY_DIR}/md_pi0w_blobs.S" @ONLY)

set(MD_PI0W_DOS_BASE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/start.S"
    "${CMAKE_CURRENT_LIST_DIR}/microdos_pi0w.c"
    "${CMAKE_CURRENT_LIST_DIR}/mini_libc.c"
    "${CMAKE_CURRENT_BINARY_DIR}/md_pi0w_blobs.S"

    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/exec_router.c"
    "${MD_ROOT}/src/system/md_dos2_system.c"
    "${MD_ROOT}/src/host/msdos2_boot.c"
)

function(md_pi0w_dos target output kernel_aot app_aot jit profile direct jit_bytes)
    add_executable(${target} ${MD_PI0W_DOS_BASE_SOURCES})

    if(jit)
        target_sources(${target} PRIVATE
            "${MD_ROOT}/src/runtime/region.c"
            "${MD_ROOT}/src/runtime/jit_core.c"
            "${MD_ROOT}/src/decode/x86_decode.c")
    elseif(kernel_aot OR app_aot)
        # Generated AOT can call resident region helpers.
        target_sources(${target} PRIVATE "${MD_ROOT}/src/runtime/region.c")
    endif()

    if(kernel_aot OR app_aot)
        # Generated AOT contains a link-time fallback reference to the decoded
        # cache runner. The runtime cache remains disabled for this target.
        target_sources(${target} PRIVATE "${MD_ROOT}/src/runtime/x86_block_cache.c")
    endif()

    if(app_aot)
        target_sources(${target} PRIVATE "${MD_GENERATED}/dos2test_recomp.c")
    endif()
    if(kernel_aot)
        target_sources(${target} PRIVATE "${MD_GENERATED}/msdos2_recomp.c")
    endif()

    if(kernel_aot OR app_aot)
        set(enable_aot 1)
    else()
        set(enable_aot 0)
    endif()

    target_include_directories(${target} PRIVATE
        "${MD_ROOT}/include"
        "${MD_ROOT}/src/runtime"
        "${MD_ROOT}/src/system"
        "${MD_ROOT}/src/host"
        "${MD_GENERATED}"
    )

    target_compile_definitions(${target} PRIVATE
        NDEBUG=1
        MD_THREADED_DISPATCH=1
        MICRODOS_PI0W_BAREMETAL=1
        MICRODOS_PI0W_JIT=${jit}
        MICRODOS_PI0W_KERNEL_AOT=${kernel_aot}
        MICRODOS_PI0W_DOS2TEST_AOT=${app_aot}
        MICRODOS_SYSTEM_ENABLE_AOT=${enable_aot}
        MICRODOS_SYSTEM_ENABLE_CACHE=0
        MD_EXEC_PROFILE=${profile}
    )

    if(jit)
        target_compile_definitions(${target} PRIVATE
            MICRODOS_ENABLE_JIT=1
            MICRODOS_PI0W_JIT_AARCH64=1
            MICRODOS_SYSTEM_JIT_CODE_BYTES=${jit_bytes}
            MD_JIT_BLOCK_SLOTS=64
            MD_JIT_LEGACY_HOTNESS=0
            MD_JIT_PROFILE=${profile}
            MD_EXEC_ENABLE_PROMOTION=1
            MD_EXEC_ENABLE_DIRECT=${direct})
    endif()

    target_compile_options(${target} PRIVATE
        -O2
        -Wall
        -Wextra
        -Wno-unused-parameter
        -ffunction-sections
        -fdata-sections
    )

    string(REPLACE ".elf" "" map_name "${target}")
    target_link_options(${target} PRIVATE
        -nostdlib
        -static
        "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
        -Wl,--gc-sections
        -Wl,--build-id=none
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${map_name}.map"
    )

    target_link_libraries(${target} PRIVATE gcc)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary
                "$<TARGET_FILE:${target}>"
                "${CMAKE_CURRENT_BINARY_DIR}/${output}"
        COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${target}>"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${output}"
        COMMENT "Producing ${output}"
    )
endfunction()

md_pi0w_dos(microdos_pi0w.elf kernel8.img 1 1 1 1 1 24576)

# True minimum baseline: canonical threaded interpreter only.
md_pi0w_dos(microdos_pi0w_threaded.elf kernel8_threaded.img 0 0 0 0 0 0)
target_compile_definitions(microdos_pi0w_threaded.elf PRIVATE
    MD_X86_TRACK_WRITES=0
    MICRODOS_TRANSLATION_SUPPORT=0)

# Temporary opcode-mix profiler: same minimal interpreter, counters only.
md_pi0w_dos(microdos_pi0w_threaded_profile.elf kernel8_threaded_profile.img 0 0 0 0 0 0)
target_compile_definitions(microdos_pi0w_threaded_profile.elf PRIVATE
    MD_X86_TRACK_WRITES=0
    MICRODOS_TRANSLATION_SUPPORT=0
    MD_INTERP_OPCODE_PROFILE=1)


# Adaptive candidates.
md_pi0w_dos(microdos_pi0w_hybrid.elf kernel8_hybrid.img 0 0 1 0 0 12288)
md_pi0w_dos(microdos_pi0w_hybrid_profile.elf kernel8_hybrid_profile.img 0 0 1 1 0 12288)


# ---------------------------------------------------------------------------
# Standalone Pi benchmark image.  It shares start.S, the runtime/JIT core and
# generated AOT fixtures with the normal DOS image, but has its own kernel_main.
# The normal kernel8.img is left untouched; this target emits kernel8_bench.img.
# ---------------------------------------------------------------------------

add_executable(microdos_pi0w_bench.elf
    "${CMAKE_CURRENT_LIST_DIR}/start.S"
    "${CMAKE_CURRENT_LIST_DIR}/microdos_pi0w_bench.c"
    "${CMAKE_CURRENT_LIST_DIR}/mini_libc.c"

    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/x86_block_cache.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/exec_router.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
    "${MD_ROOT}/src/decode/x86_decode.c"

    "${MD_GENERATED}/loop_recomp.c"
    "${MD_GENERATED}/memloop_recomp.c"
    "${MD_GENERATED}/checksum_recomp.c"
)

target_include_directories(microdos_pi0w_bench.elf PRIVATE
    "${MD_ROOT}/include"
    "${MD_ROOT}/src/runtime"
    "${MD_GENERATED}"
)

target_compile_definitions(microdos_pi0w_bench.elf PRIVATE
    NDEBUG=1
    MD_THREADED_DISPATCH=1
    MICRODOS_PI0W_BAREMETAL=1
    MICRODOS_PI0W_JIT_AARCH64=1
    MD_JIT_PROFILE=1
)

target_compile_options(microdos_pi0w_bench.elf PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
    -ffunction-sections
    -fdata-sections
)

target_link_options(microdos_pi0w_bench.elf PRIVATE
    -nostdlib
    -static
    "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
    -Wl,--gc-sections
    -Wl,--build-id=none
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/microdos_pi0w_bench.map"
)

target_link_libraries(microdos_pi0w_bench.elf PRIVATE gcc)

add_custom_command(TARGET microdos_pi0w_bench.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary
            "$<TARGET_FILE:microdos_pi0w_bench.elf>"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel8_bench.img"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:microdos_pi0w_bench.elf>"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/kernel8_bench.img"
    COMMENT "Producing Pi Zero 2 W benchmark kernel8_bench.img"
)

# ---------------------------------------------------------------------------
# Native-3 omnibus targets (N3.0-N3.8). Parallel to legacy hybrid JIT.
# ---------------------------------------------------------------------------
md_pi0w_dos(microdos_pi0w_native3.elf kernel8_native3.img 0 0 0 0 0 0)
target_sources(microdos_pi0w_native3.elf PRIVATE
    "${MD_ROOT}/src/runtime/native3.c"
    "${MD_ROOT}/src/runtime/native_v2.c"
    "${MD_ROOT}/src/runtime/native_v2_runtime.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
    "${MD_ROOT}/src/decode/x86_decode.c")
target_compile_definitions(microdos_pi0w_native3.elf PRIVATE
    MICRODOS_ENABLE_NATIVE3=1
    MICRODOS_NATIVE3_A64_DIRECT_EXT=1
    MD_N3_PROFILE=0
    MD_JIT_PROFILE=0
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_BLOCK_SLOTS=256
    "MICRODOS_NATIVE3_CODE_BYTES=(128u*1024u)")

add_executable(microdos_pi0w_native3_bench.elf
    "${CMAKE_CURRENT_LIST_DIR}/start.S"
    "${CMAKE_CURRENT_LIST_DIR}/microdos_pi0w_native3_bench.c"
    "${CMAKE_CURRENT_LIST_DIR}/mini_libc.c"
    "${MD_ROOT}/src/runtime/native3.c"
    "${MD_ROOT}/src/runtime/native_v2.c"
    "${MD_ROOT}/src/runtime/native_v2_runtime.c"
    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
    "${MD_ROOT}/src/decode/x86_decode.c")
target_include_directories(microdos_pi0w_native3_bench.elf PRIVATE
    "${MD_ROOT}/include" "${MD_ROOT}/src/runtime")
target_compile_definitions(microdos_pi0w_native3_bench.elf PRIVATE
    NDEBUG=1
    MD_THREADED_DISPATCH=1
    MICRODOS_PI0W_BAREMETAL=1
    MICRODOS_NATIVE3_A64_DIRECT_EXT=1
    MD_N3_PROFILE=1
    MD_JIT_PROFILE=1
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_BLOCK_SLOTS=256)
target_compile_options(microdos_pi0w_native3_bench.elf PRIVATE
    -O3 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)
target_link_options(microdos_pi0w_native3_bench.elf PRIVATE
    -nostdlib -static "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
    -Wl,--gc-sections -Wl,--build-id=none
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/microdos_pi0w_native3_bench.map")
target_link_libraries(microdos_pi0w_native3_bench.elf PRIVATE gcc)
add_custom_command(TARGET microdos_pi0w_native3_bench.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary
        "$<TARGET_FILE:microdos_pi0w_native3_bench.elf>"
        "${CMAKE_CURRENT_BINARY_DIR}/kernel8_native3_bench.img"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:microdos_pi0w_native3_bench.elf>"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/kernel8_native3_bench.img"
    COMMENT "Producing Native-3 kernel8_native3_bench.img")

# Optional blitz86 external Pi probe
include("${MD_ROOT}/pi0w/blitz86_pi_target.cmake")

# Optional blitz86 native JIT diagnostic target
include("${MD_ROOT}/pi0w/blitz86_pi_jit_target.cmake")

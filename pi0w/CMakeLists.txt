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

add_executable(microdos_pi0w.elf
    "${CMAKE_CURRENT_LIST_DIR}/start.S"
    "${CMAKE_CURRENT_LIST_DIR}/microdos_pi0w.c"
    "${CMAKE_CURRENT_LIST_DIR}/mini_libc.c"
    "${CMAKE_CURRENT_BINARY_DIR}/md_pi0w_blobs.S"

    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/x86_block_cache.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/exec_router.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
    "${MD_ROOT}/src/decode/x86_decode.c"
    "${MD_ROOT}/src/system/md_dos2_system.c"
    "${MD_ROOT}/src/host/msdos2_boot.c"

    "${MD_GENERATED}/dos2test_recomp.c"
    "${MD_GENERATED}/msdos2_recomp.c"
)

target_include_directories(microdos_pi0w.elf PRIVATE
    "${MD_ROOT}/include"
    "${MD_ROOT}/src/runtime"
    "${MD_ROOT}/src/system"
    "${MD_ROOT}/src/host"
    "${MD_GENERATED}"
)

target_compile_definitions(microdos_pi0w.elf PRIVATE
    NDEBUG=1
    MD_THREADED_DISPATCH=1
    MICRODOS_PI0W_BAREMETAL=1

    # AArch64 JIT v1:
    #
    # Shared JIT/router policy remains common with Pico. jit_core.c selects
    # jit_aarch64_backend.inc on __aarch64__. The first native slice emits
    # A64 direct prefixes plus small resident-helper trampolines.
    MICRODOS_ENABLE_JIT=1
    MICRODOS_PI0W_JIT_AARCH64=1
    MICRODOS_SYSTEM_JIT_CODE_BYTES=24576
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_PROFILE=1
    MD_EXEC_PROFILE=1
    MD_EXEC_ENABLE_PROMOTION=1
    MD_EXEC_ENABLE_DIRECT=1
)

target_compile_options(microdos_pi0w.elf PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
    -ffunction-sections
    -fdata-sections
)

target_link_options(microdos_pi0w.elf PRIVATE
    -nostdlib
    -static
    "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
    -Wl,--gc-sections
    -Wl,--build-id=none
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/microdos_pi0w.map"
)

# libgcc supplies any compiler runtime helpers emitted by GCC; no hosted libc
# or operating-system ABI is linked.
target_link_libraries(microdos_pi0w.elf PRIVATE gcc)

add_custom_command(TARGET microdos_pi0w.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary
            "$<TARGET_FILE:microdos_pi0w.elf>"
            "${CMAKE_CURRENT_BINARY_DIR}/kernel8.img"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:microdos_pi0w.elf>"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/kernel8.img"
    COMMENT "Producing Raspberry Pi Zero 2 W bare-metal kernel8.img"
)


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

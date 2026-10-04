# microDOS for Raspberry Pi Pico 2 / Pimoroni Pico Plus 2 (RP2350, 8 MiB PSRAM).
#
# Built by `.\md.bat build pico`, which first builds the desktop tools
# (dosrecomp, mkfat12) and a fresh disk image, then configures this project.
#
# Inputs (all overridable with -D):
#   MICRODOS_HOST_BUILD   desktop build dir holding generated/dos2test_recomp.c
#   MICRODOS_PICO_KERNEL  released MSDOS.SYS (16690 bytes)
#   MICRODOS_PICO_DISK    360 KiB FAT12 image embedded in flash
#
# Mirrors microconsole: Pico SDK 2.3.0+ (hardware_psram), explicit
# PICO_SDK_PATH wins over the VS Code extension helper.
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{PICO_SDK_PATH} AND NOT "$ENV{PICO_SDK_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{PICO_SDK_PATH}" MD_EXPLICIT_PICO_SDK_PATH)
    set(PICO_SDK_PATH "${MD_EXPLICIT_PICO_SDK_PATH}" CACHE PATH "Pico SDK" FORCE)
else()
    if(WIN32)
        set(USERHOME $ENV{USERPROFILE})
    else()
        set(USERHOME $ENV{HOME})
    endif()
    set(sdkVersion 2.3.0)
    set(toolchainVersion 14_2_Rel1)
    set(picotoolVersion 2.3.0)
    set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
    if(EXISTS ${picoVscode})
        include(${picoVscode})
    endif()
endif()

# Use the picotool installed by the VS Code Pico extension when present, so
# the SDK never tries to build picotool from source (that needs a host C++
# compiler, which a plain Ninja + ARM GCC environment does not have).
if(NOT DEFINED picotool_DIR)
    if(WIN32)
        set(MD_PICO_HOME $ENV{USERPROFILE})
    else()
        set(MD_PICO_HOME $ENV{HOME})
    endif()
    if(EXISTS "${MD_PICO_HOME}/.pico-sdk/picotool/2.3.0/picotool")
        set(picotool_DIR "${MD_PICO_HOME}/.pico-sdk/picotool/2.3.0/picotool")
    endif()
endif()

set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
project(microdos_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
pico_sdk_init()

set(MD_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(MICRODOS_HOST_BUILD "${MD_ROOT}/build-host" CACHE PATH "Desktop build directory")
set(MICRODOS_PICO_KERNEL "${MD_ROOT}/third_party/msdos/v2.0/bin/MSDOS.SYS" CACHE FILEPATH "MSDOS.SYS")
set(MICRODOS_PICO_DISK "${MD_ROOT}/build-pico/pico_disk.img" CACHE FILEPATH "FAT12 image")

set(MD_GENERATED "${MICRODOS_HOST_BUILD}/generated")
foreach(req "${MD_GENERATED}/dos2test_recomp.c" "${MD_GENERATED}/msdos2_recomp.c" "${MD_GENERATED}/loop_recomp.c"
            "${MD_GENERATED}/memloop_recomp.c" "${MD_GENERATED}/checksum_recomp.c"
            "${MICRODOS_PICO_KERNEL}" "${MICRODOS_PICO_DISK}")
    if(NOT EXISTS "${req}")
        message(FATAL_ERROR "microDOS Pico: missing ${req}\n"
                            "Run .\\md.bat build pico (it builds the host tools and disk first).")
    endif()
endforeach()

if(NOT TARGET hardware_psram)
    message(FATAL_ERROR "microDOS Pico needs Pico SDK 2.3.0+ (hardware_psram)")
endif()

file(TO_CMAKE_PATH "${MICRODOS_PICO_KERNEL}" MICRODOS_PICO_KERNEL)
file(TO_CMAKE_PATH "${MICRODOS_PICO_DISK}" MICRODOS_PICO_DISK)
configure_file("${CMAKE_CURRENT_LIST_DIR}/md_pico_blobs.S.in"
               "${CMAKE_CURRENT_BINARY_DIR}/md_pico_blobs.S" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/md_pico_blobs.S" PROPERTIES
    OBJECT_DEPENDS "${MICRODOS_PICO_KERNEL};${MICRODOS_PICO_DISK}")

set(MD_RUNTIME_BASE_SOURCES
    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/region.c"
    "${MD_ROOT}/src/runtime/exec_router.c")

function(md_pico_common target code_in_sram)
    target_include_directories(${target} PRIVATE
        "${MD_ROOT}/include" "${MD_ROOT}/src/runtime" "${MD_ROOT}/src/host"
        "${MD_ROOT}/src/system" "${MD_GENERATED}")
    target_compile_definitions(${target} PRIVATE
        MD_THREADED_DISPATCH=1
        PICO_STDIO_DEFAULT_CRLF=0                 # DOS already sends CR LF
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
        MICRODOS_PICO_CODE_IN_SRAM=${code_in_sram})
    target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${target} PRIVATE pico_stdlib hardware_psram hardware_clocks)
    if(code_in_sram)
        # All code and read-only data run from SRAM; only .flashdata (the
        # MSDOS.SYS and disk blobs) stays in flash. Frees the 16 KiB XIP
        # cache for PSRAM guest memory.
        pico_set_binary_type(${target} copy_to_ram)
    endif()
    pico_enable_stdio_usb(${target} 1)
    pico_enable_stdio_uart(${target} 0)
    pico_add_extra_outputs(${target})
    target_link_options(${target} PRIVATE
        "-Wl,--print-memory-usage"
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
endfunction()

# DOS firmware: name, code_in_sram, block_cache, sys_khz (0 = SDK default),
# kernel_aot (1 = link compiled MSDOS.SYS), jit (1 = runtime translator for
# segments with no matching static AOT image).
function(md_pico_dos target code_in_sram cache sys_khz kernel_aot app_aot jit promote direct jit_bytes)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/microdos_pico.c"
        "${CMAKE_CURRENT_BINARY_DIR}/md_pico_blobs.S"
        "${MD_ROOT}/src/system/md_dos2_system.c"
        "${MD_ROOT}/src/host/msdos2_boot.c"
        ${MD_RUNTIME_BASE_SOURCES})

    if(cache OR kernel_aot OR app_aot)
        # dosrecomp AOT fallback contains a link-time reference to
        # md_interp_run_cached_until(). AOT references therefore need the
        # implementation even when the runtime cache itself is disabled.
        target_sources(${target} PRIVATE "${MD_ROOT}/src/runtime/x86_block_cache.c")
    endif()
    if(app_aot)
        target_sources(${target} PRIVATE "${MD_GENERATED}/dos2test_recomp.c")
    endif()
    if(kernel_aot)
        target_sources(${target} PRIVATE "${MD_GENERATED}/msdos2_recomp.c")
    endif()
    if(jit)
        target_sources(${target} PRIVATE
            "${MD_ROOT}/src/runtime/jit_core.c"
            "${MD_ROOT}/src/decode/x86_decode.c")
    endif()

    if(kernel_aot OR app_aot)
        set(enable_aot 1)
    else()
        set(enable_aot 0)
    endif()

    md_pico_common(${target} ${code_in_sram})
    target_compile_definitions(${target} PRIVATE
        MICRODOS_PICO_CACHE=${cache}
        MICRODOS_PICO_SYS_KHZ=${sys_khz}
        MICRODOS_PICO_KERNEL_AOT=${kernel_aot}
        MICRODOS_PICO_DOS2TEST_AOT=${app_aot}
        MICRODOS_PICO_JIT=${jit}
        MICRODOS_SYSTEM_ENABLE_CACHE=${cache}
        MICRODOS_SYSTEM_ENABLE_AOT=${enable_aot})

    if(jit)
        target_compile_definitions(${target} PRIVATE
            MICRODOS_ENABLE_JIT=1
            MD_JIT_LEGACY_HOTNESS=0
            MD_JIT_BLOCK_SLOTS=64
            MD_JIT_HOT_THRESHOLD=2
            MD_JIT_HOTNESS_SLOTS=64
            MICRODOS_SYSTEM_JIT_CODE_BYTES=${jit_bytes}
            MD_EXEC_ENABLE_PROMOTION=${promote}
            MD_EXEC_ENABLE_DIRECT=${direct})
        pico_set_program_name(${target} "microDOS JIT")
    else()
        target_compile_definitions(${target} PRIVATE MD_EXEC_PROFILE=0)
        pico_set_program_name(${target} "microDOS")
    endif()
endfunction()

# Benchmark firmware: name, code_in_sram, sys_khz
# M19 adds the runtime Thumb translator plus the shared structural decoder.
# Native v2 architecture benchmark. This deliberately does not link
# the old runtime JIT or AOT generated sources.
function(md_pico_nativev2_bench target code_in_sram sys_khz)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/microdos_nativev2_bench.c"
        "${MD_ROOT}/src/runtime/native_v2.c"
        "${MD_ROOT}/src/decode/x86_decode.c")
    md_pico_common(${target} ${code_in_sram})
    target_compile_definitions(${target} PRIVATE
        MICRODOS_PICO_SYS_KHZ=${sys_khz})
    pico_set_program_name(${target} "microDOS Native v2")
endfunction()

function(md_pico_bench target code_in_sram sys_khz)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/microdos_bench.c"
        ${MD_RUNTIME_BASE_SOURCES}
        "${MD_ROOT}/src/runtime/x86_block_cache.c"
        "${MD_ROOT}/src/runtime/jit_core.c"
        "${MD_ROOT}/src/decode/x86_decode.c"
        "${MD_GENERATED}/loop_recomp.c"
        "${MD_GENERATED}/memloop_recomp.c"
        "${MD_GENERATED}/checksum_recomp.c")
    md_pico_common(${target} ${code_in_sram})
    target_compile_definitions(${target} PRIVATE MICRODOS_PICO_SYS_KHZ=${sys_khz})
    pico_set_program_name(${target} "microDOS bench")
endfunction()

# Explicit size/performance comparison images.
md_pico_dos(microdos_pico          1 0 300000 1 1 0 0 0     0)
md_pico_dos(microdos_pico_150      1 0      0 1 1 0 0 0     0)

# Pure execution baselines.
md_pico_dos(microdos_pico_nokernel 1 0 300000 0 0 0 0 0     0)
target_compile_definitions(microdos_pico_nokernel PRIVATE
    MD_X86_TRACK_WRITES=0
    MICRODOS_TRANSLATION_SUPPORT=0)
md_pico_dos(microdos_pico_region   1 1 300000 0 0 0 0 0     0)
target_compile_definitions(microdos_pico_region PRIVATE MD_CACHE_PROFILE=0)

# Minimal adaptive candidate: threaded by default, JIT only proven hot regions.
md_pico_dos(microdos_pico_jitkernel 1 0 300000 0 0 1 1 0 12288)
target_compile_definitions(microdos_pico_jitkernel PRIVATE
    MD_JIT_PROFILE=0 MD_EXEC_PROFILE=0)

# AOT + adaptive region-JIT comparison.
md_pico_dos(microdos_pico_jit 1 0 300000 1 1 1 1 0 24576)
target_compile_definitions(microdos_pico_jit PRIVATE
    MD_JIT_PROFILE=0 MD_EXEC_PROFILE=0)

# Instrumented build of the same no-AOT adaptive policy.
md_pico_dos(microdos_pico_jit_profile 1 0 300000 0 0 1 1 0 12288)
target_compile_definitions(microdos_pico_jit_profile PRIVATE
    MD_JIT_PROFILE=1 MD_EXEC_PROFILE=1)

# M21.1 memory-placement A/B: identical firmwares (compiled kernel, no cache,
# no JIT, 300 MHz, 128 KiB guest, DOS told it has 128 KiB, DOS2TEST not
# precompiled so the SRAM variant fits) except for where guest RAM lives.
# DOS2TEST's MIPS difference is the real PSRAM penalty for DOS.
md_pico_dos(microdos_pico_g128_psram 1 0 300000 1 0 0 0 0 0)
md_pico_dos(microdos_pico_g128_sram  1 0 300000 1 0 0 0 0 0)
foreach(t microdos_pico_g128_psram microdos_pico_g128_sram)
    target_compile_definitions(${t} PRIVATE MD_X86_ADDRESS_BITS=17u
        MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS=0x2000u)
endforeach()
target_compile_definitions(microdos_pico_g128_psram PRIVATE MICRODOS_PICO_GUEST_SRAM=0)
target_compile_definitions(microdos_pico_g128_sram  PRIVATE MICRODOS_PICO_GUEST_SRAM=1)

md_pico_nativev2_bench(microdos_nativev2_bench 1 300000)

# Native v2 USB-only smoke test: no decoder/compiler/generated code and
# no overclock. Used to isolate USB stdio from the native engine.
add_executable(microdos_nativev2_usbsmoke
    "${CMAKE_CURRENT_LIST_DIR}/microdos_nativev2_usbsmoke.c")
md_pico_common(microdos_nativev2_usbsmoke 1)
target_compile_definitions(microdos_nativev2_usbsmoke PRIVATE
    MICRODOS_PICO_SYS_KHZ=0)
pico_set_program_name(microdos_nativev2_usbsmoke "microDOS NV2 USB smoke")

md_pico_bench(microdos_bench     1 300000)
md_pico_bench(microdos_bench_150 1 0)

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
set(MICRODOS_PICO_SYS_KHZ 0 CACHE STRING "clk_sys in kHz (0 = SDK default)")

set(MD_GENERATED "${MICRODOS_HOST_BUILD}/generated")
foreach(req "${MD_GENERATED}/dos2test_recomp.c" "${MICRODOS_PICO_KERNEL}" "${MICRODOS_PICO_DISK}")
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

add_executable(microdos_pico
    "${CMAKE_CURRENT_LIST_DIR}/microdos_pico.c"
    "${CMAKE_CURRENT_BINARY_DIR}/md_pico_blobs.S"
    "${MD_ROOT}/src/system/md_dos2_system.c"
    "${MD_ROOT}/src/host/msdos2_boot.c"
    "${MD_ROOT}/src/runtime/runtime.c"
    "${MD_ROOT}/src/runtime/x86_interp.c"
    "${MD_ROOT}/src/runtime/x86_block_cache.c"
    "${MD_GENERATED}/dos2test_recomp.c")

target_include_directories(microdos_pico PRIVATE
    "${MD_ROOT}/include" "${MD_ROOT}/src/runtime" "${MD_ROOT}/src/host"
    "${MD_ROOT}/src/system" "${MD_GENERATED}")

target_compile_definitions(microdos_pico PRIVATE
    MD_THREADED_DISPATCH=1
    PICO_STDIO_DEFAULT_CRLF=0                 # DOS already sends CR LF
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    MICRODOS_PICO_SYS_KHZ=${MICRODOS_PICO_SYS_KHZ})
target_compile_options(microdos_pico PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(microdos_pico PRIVATE pico_stdlib hardware_psram hardware_clocks)
pico_enable_stdio_usb(microdos_pico 1)
pico_enable_stdio_uart(microdos_pico 0)
pico_set_program_name(microdos_pico "microDOS")
pico_add_extra_outputs(microdos_pico)
target_link_options(microdos_pico PRIVATE "-Wl,--print-memory-usage")

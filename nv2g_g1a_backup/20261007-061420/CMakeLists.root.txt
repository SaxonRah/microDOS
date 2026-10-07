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
    src/runtime/region.c
    src/runtime/exec_router.c
    src/runtime/translate.c
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
    set_property(GLOBAL APPEND PROPERTY MD_GENERATED_OUTPUTS "${out_c}" "${out_h}")
endfunction()

md_recompile_com(hello md_recomp_hello tests/programs/hello.com 0x10c)
md_recompile_com(loop md_recomp_loop tests/programs/loop.com 0x107)
# 0105h MOV BX,AX is compiled by dosrecomp v2; pin it as an interpreter hole so
# this fixture keeps proving the AOT -> cached interpreter -> AOT handoff.
md_recompile_com(hybrid md_recomp_hybrid tests/programs/hybrid.com 0x109 --interp-at 0x105)
md_recompile_com(selfmod md_recomp_selfmod tests/programs/selfmod.com 0x107)
md_recompile_com(memloop md_recomp_memloop tests/programs/memloop.com 0x117)
# M21 convergence fixture: 32 KiB LODSW/ADD/LOOP checksum.
md_recompile_com(checksum md_recomp_checksum tests/programs/checksum.com 0x10f)
md_recompile_com(xchgself md_recomp_xchgself tests/programs/xchgself.com 0x107)
md_recompile_com(divfault md_recomp_divfault tests/programs/divfault.com 0x113)

# M13: DOS2TEST.COM compiled natively and attached when DOS loads it at A>.
# Entry points come from the NASM listing (tests reached via CALL BX).
# M17: the released MS-DOS 2.0 kernel itself, compiled when the pinned
# Microsoft binaries are present (.\md.bat deps msdos). Raw image at 0000h;
# entry points = profile-guided list + the INT 21h dispatch table.
set(MD_MSDOS2_SYS "${CMAKE_CURRENT_LIST_DIR}/third_party/msdos/v2.0/bin/MSDOS.SYS")
if(EXISTS "${MD_MSDOS2_SYS}")
    set(MD_HAVE_KERNEL_AOT ON)
    set(MD_RECOMP_EXTRA_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/aot/msdos2.entries")
    md_recompile_com(msdos2 md_recomp_msdos2 third_party/msdos/v2.0/bin/MSDOS.SYS 0x4132
        --base 0 --entries "${CMAKE_CURRENT_LIST_DIR}/aot/msdos2.entries" --name MSDOS.SYS)
    unset(MD_RECOMP_EXTRA_DEPENDS)
else()
    set(MD_HAVE_KERNEL_AOT OFF)
endif()

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
    if(MD_HAVE_KERNEL_AOT)
        target_sources(microdos_msdos2 PRIVATE "${MD_RECOMP_msdos2_C}" "${MD_RECOMP_msdos2_H}")
        target_compile_definitions(microdos_msdos2 PRIVATE MICRODOS_HAVE_KERNEL_AOT=1)
    endif()
    target_include_directories(microdos_msdos2 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")
    if(MSVC)
        target_compile_definitions(microdos_msdos2 PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(microdos_msdos2 PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(microdos_msdos2 PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3>)
    endif()

    add_executable(microdos_bench src/host/bench.c
        "${MD_RECOMP_loop_C}" "${MD_RECOMP_loop_H}"
        "${MD_RECOMP_checksum_C}" "${MD_RECOMP_checksum_H}")
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
        "${MD_RECOMP_selfmod_C}" "${MD_RECOMP_selfmod_H}"
        "${MD_RECOMP_memloop_C}" "${MD_RECOMP_memloop_H}"
        "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_dos2test_H}"
        "${MD_RECOMP_xchgself_C}" "${MD_RECOMP_xchgself_H}"
        "${MD_RECOMP_divfault_C}" "${MD_RECOMP_divfault_H}")
    target_link_libraries(microdos_tests PRIVATE microdos::runtime)
    target_include_directories(microdos_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/host" "${MD_GENERATED_DIR}")
    add_test(NAME microdos_runtime COMMAND microdos_tests)
    add_test(NAME dosprobe_selftest COMMAND dosprobe --selftest)
    add_test(NAME mkfat12_selftest COMMAND mkfat12 --selftest)

    # Runtime JIT differential fixtures (M20.2.2/M20.3). On the host this checks the
    # JIT's decode/region logic and C reference semantics against the
    # interpreter; tools/jit_qemu_check.sh runs the same test as native
    # Thumb-2 under qemu-arm, which also checks code shape (fallbacks,
    # resident regions). test_jit.c is the M20.3 spec (LODS/LOOP
    # resident regions); on the host its code-shape checks are skipped.
    set(MD_JIT_TEST_SOURCES src/runtime/jit_core.c)
    add_executable(microdos_jit_diff tests/test_jit_diff.c ${MD_JIT_TEST_SOURCES})
    target_link_libraries(microdos_jit_diff PRIVATE microdos::runtime microdos::decode)
    target_include_directories(microdos_jit_diff PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/runtime")
    add_test(NAME jit_diff COMMAND microdos_jit_diff)
    # M21.1b: restructured store tracking vs the original algorithm.
    add_executable(microdos_store_tracking tests/test_store_tracking.c)
    target_link_libraries(microdos_store_tracking PRIVATE microdos::runtime)
    add_test(NAME store_tracking COMMAND microdos_store_tracking)
    add_executable(microdos_jit_m203 tests/test_jit.c ${MD_JIT_TEST_SOURCES})
    target_link_libraries(microdos_jit_m203 PRIVATE microdos::runtime microdos::decode)
    target_include_directories(microdos_jit_m203 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/runtime")
    add_test(NAME jit_m203 COMMAND microdos_jit_m203)

    # M21 shared resident-region semantics.  This compares the O(1) DEC/JNZ
    # region and resident LODSW/ADD/LOOP region against the canonical
    # interpreter, including DF and the 20-bit physical wrap.
    add_executable(microdos_region_tests tests/test_region.c)
    target_link_libraries(microdos_region_tests PRIVATE microdos::runtime)
    add_test(NAME region COMMAND microdos_region_tests)

    # Native v2: host-side decode/lowering/code-shape test. Generated
    # Thumb bytes are not executed on the desktop.
    add_executable(microdos_native_v2_tests
        tests/test_native_v2.c src/runtime/native_v2.c)
    target_link_libraries(microdos_native_v2_tests PRIVATE microdos::decode)
    target_include_directories(microdos_native_v2_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include")
    add_test(NAME native_v2_phase1 COMMAND microdos_native_v2_tests)

    # Generated AOT must produce the same checksum result/count/flags.
    add_executable(microdos_region_aot_tests tests/test_region_aot.c
        "${MD_RECOMP_checksum_C}" "${MD_RECOMP_checksum_H}")
    target_link_libraries(microdos_region_aot_tests PRIVATE microdos::runtime)
    target_include_directories(microdos_region_aot_tests PRIVATE "${MD_GENERATED_DIR}")
    add_test(NAME region_aot COMMAND microdos_region_aot_tests)

    # M23: pure policy, and real system-loop differential tests in baseline,
    # promotion and lean configurations. Compile each layout consistently.
    add_executable(microdos_exec_router tests/test_exec_router.c)
    target_link_libraries(microdos_exec_router PRIVATE microdos::runtime)
    add_test(NAME exec_router COMMAND microdos_exec_router)
    add_executable(microdos_exec_router_lean tests/test_exec_router.c src/runtime/exec_router.c)
    target_include_directories(microdos_exec_router_lean PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_definitions(microdos_exec_router_lean PRIVATE MD_EXEC_PROFILE=0)
    add_test(NAME exec_router_lean COMMAND microdos_exec_router_lean)
    foreach(variant baseline promote lean)
        add_executable(microdos_exec_router_diff_${variant}
            tests/test_exec_router_diff.c src/system/md_dos2_system.c
            src/host/msdos2_boot.c src/runtime/jit_core.c
            src/runtime/runtime.c src/runtime/x86_interp.c
            src/runtime/x86_block_cache.c src/runtime/region.c src/runtime/exec_router.c
            "${MD_RECOMP_loop_C}")
        target_link_libraries(microdos_exec_router_diff_${variant} PRIVATE microdos::decode)
        target_include_directories(microdos_exec_router_diff_${variant} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src/runtime"
            "${CMAKE_CURRENT_LIST_DIR}/src/host" "${CMAKE_CURRENT_LIST_DIR}/src/system" "${MD_GENERATED_DIR}")
        target_compile_definitions(microdos_exec_router_diff_${variant} PRIVATE MICRODOS_ENABLE_JIT=1)
        if(NOT variant STREQUAL "baseline")
            target_compile_definitions(microdos_exec_router_diff_${variant} PRIVATE
                MD_EXEC_ENABLE_PROMOTION=1 MD_EXEC_ENABLE_DIRECT=1)
        endif()
        if(variant STREQUAL "lean")
            target_compile_definitions(microdos_exec_router_diff_${variant} PRIVATE MD_JIT_PROFILE=0 MD_EXEC_PROFILE=0 MD_JIT_LEGACY_HOTNESS=0)
        endif()
        if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MD_FORCE_SWITCH_DISPATCH)
            target_compile_definitions(microdos_exec_router_diff_${variant} PRIVATE MD_THREADED_DISPATCH=1)
        endif()
        if(MSVC)
            target_compile_definitions(microdos_exec_router_diff_${variant} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_dependencies(microdos_exec_router_diff_${variant} md_generated)
        add_test(NAME exec_router_diff_${variant} COMMAND microdos_exec_router_diff_${variant})
    endforeach()

    # M14 end-to-end: boot the released MS-DOS 2.0 through the portable
    # system loop (the code the Pico 2 runs) and pass DOS2TEST at A>.
    # Needs the pinned Microsoft binaries (.\md.bat deps msdos).
    set(MD_MSDOS2_BIN "${CMAKE_CURRENT_LIST_DIR}/third_party/msdos/v2.0/bin")
    if(EXISTS "${MD_MSDOS2_BIN}/MSDOS.SYS" AND EXISTS "${MD_MSDOS2_BIN}/COMMAND.COM")
        add_executable(microdos_dos2_e2e tests/test_dos2_e2e.c
            src/system/md_dos2_system.c src/host/msdos2_boot.c
            "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_dos2test_H}"
            "${MD_RECOMP_msdos2_C}" "${MD_RECOMP_msdos2_H}")
        target_link_libraries(microdos_dos2_e2e PRIVATE microdos::runtime microdos::decode)
        target_include_directories(microdos_dos2_e2e PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/src/host" "${CMAKE_CURRENT_LIST_DIR}/src/system" "${MD_GENERATED_DIR}")
        if(MSVC)
            target_compile_definitions(microdos_dos2_e2e PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        set(MD_E2E_IMG "${CMAKE_CURRENT_BINARY_DIR}/e2e_msdos2.img")
        add_test(NAME dos2_e2e_image COMMAND mkfat12
            --command "${MD_MSDOS2_BIN}/COMMAND.COM"
            --add "${CMAKE_CURRENT_LIST_DIR}/tests/dos2/DOS2TEST.COM" DOS2TEST.COM
            --output "${MD_E2E_IMG}")
        set_tests_properties(dos2_e2e_image PROPERTIES FIXTURES_SETUP dos2_image)
        add_test(NAME dos2_e2e_aot COMMAND microdos_dos2_e2e "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}")
        add_test(NAME dos2_e2e_interp COMMAND microdos_dos2_e2e "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}" --no-aot)
        # Same configuration as the Pico *_nocache firmware (M15).
        add_test(NAME dos2_e2e_nocache COMMAND microdos_dos2_e2e "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}" --no-cache)
        # M17: compiled kernel + DOS2TEST vs pure interpreter, compared after
        # every compiled unit (CPU state) and periodically (all of memory).
        add_test(NAME dos2_e2e_lockstep COMMAND microdos_dos2_e2e "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}" --lockstep)
        # N3 integration: boot released MS-DOS 2.0 through the actual
        # MICRODOS_ENABLE_NATIVE3 system path. On x86/x64 this exercises the
        # Native-3 ownership/cache machinery with the portable semantic-oracle
        # backend; ARM/AArch64 builds may execute their real native backends.
        # AOT and the block cache are disabled for this test so Native-3 is the
        # only tier in front of the canonical interpreter.
        add_executable(microdos_dos2_e2e_native3
            tests/test_dos2_e2e.c
            src/system/md_dos2_system.c
            src/host/msdos2_boot.c
            src/runtime/native3.c
            src/runtime/native_v2.c
            src/runtime/native_v2_runtime.c
            src/runtime/jit_core.c
            "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_msdos2_C}")
        target_link_libraries(microdos_dos2_e2e_native3 PRIVATE
            microdos::runtime microdos::decode)
        target_include_directories(microdos_dos2_e2e_native3 PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${CMAKE_CURRENT_LIST_DIR}/src/runtime"
            "${CMAKE_CURRENT_LIST_DIR}/src/host"
            "${CMAKE_CURRENT_LIST_DIR}/src/system"
            "${MD_GENERATED_DIR}")
        target_compile_definitions(microdos_dos2_e2e_native3 PRIVATE
            MICRODOS_ENABLE_NATIVE3=1
            MD_N3_PROFILE=1
            MD_JIT_PROFILE=1
            MD_JIT_LEGACY_HOTNESS=0)
        if(MSVC)
            target_compile_definitions(
                microdos_dos2_e2e_native3
                PRIVATE
                _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME dos2_e2e_native3
            COMMAND microdos_dos2_e2e_native3
                "${MD_MSDOS2_BIN}/MSDOS.SYS"
                "${MD_E2E_IMG}"
                --no-aot --no-cache)
        set_tests_properties(
            dos2_e2e_native3
            PROPERTIES
            FIXTURES_REQUIRED dos2_image)

        # M21.1: the 128 KiB guest used by the Pico SRAM/PSRAM A/B firmware.
        # MdX86's code-page tables are sized from the address space, so this
        # target compiles the runtime itself instead of linking the 1 MiB
        # microdos::runtime library (mixing them would be a layout mismatch).
        add_executable(microdos_dos2_e2e_g128 tests/test_dos2_e2e.c
            src/system/md_dos2_system.c src/host/msdos2_boot.c
            src/runtime/runtime.c src/runtime/x86_interp.c
            src/runtime/x86_block_cache.c src/runtime/region.c src/runtime/exec_router.c
            "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_msdos2_C}")
        target_link_libraries(microdos_dos2_e2e_g128 PRIVATE microdos::decode)
        target_include_directories(microdos_dos2_e2e_g128 PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src/runtime"
            "${CMAKE_CURRENT_LIST_DIR}/src/host" "${CMAKE_CURRENT_LIST_DIR}/src/system" "${MD_GENERATED_DIR}")
        target_compile_definitions(microdos_dos2_e2e_g128 PRIVATE
            MD_X86_ADDRESS_BITS=17u MD_MSDOS2_DEFAULT_MEMORY_PARAGRAPHS=0x2000u)
        if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MD_FORCE_SWITCH_DISPATCH)
            target_compile_definitions(microdos_dos2_e2e_g128 PRIVATE MD_THREADED_DISPATCH=1)
        endif()
        if(MSVC)
            target_compile_definitions(microdos_dos2_e2e_g128 PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME dos2_e2e_g128 COMMAND microdos_dos2_e2e_g128 "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}" --no-cache)
        foreach(variant baseline promote lean)
            add_executable(microdos_dos2_e2e_router_${variant}
                tests/test_dos2_e2e.c src/system/md_dos2_system.c src/host/msdos2_boot.c
                src/runtime/jit_core.c src/runtime/runtime.c src/runtime/x86_interp.c
                src/runtime/x86_block_cache.c src/runtime/region.c src/runtime/exec_router.c
                "${MD_RECOMP_dos2test_C}" "${MD_RECOMP_msdos2_C}")
            target_link_libraries(microdos_dos2_e2e_router_${variant} PRIVATE microdos::decode)
            target_include_directories(microdos_dos2_e2e_router_${variant} PRIVATE
                "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src/runtime"
                "${CMAKE_CURRENT_LIST_DIR}/src/host" "${CMAKE_CURRENT_LIST_DIR}/src/system" "${MD_GENERATED_DIR}")
            target_compile_definitions(microdos_dos2_e2e_router_${variant} PRIVATE MICRODOS_ENABLE_JIT=1)
            if(NOT variant STREQUAL "baseline")
                target_compile_definitions(microdos_dos2_e2e_router_${variant} PRIVATE MD_EXEC_ENABLE_PROMOTION=1 MD_EXEC_ENABLE_DIRECT=1)
            endif()
            if(variant STREQUAL "lean")
                target_compile_definitions(microdos_dos2_e2e_router_${variant} PRIVATE MD_JIT_PROFILE=0 MD_EXEC_PROFILE=0 MD_JIT_LEGACY_HOTNESS=0)
            endif()
            if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang") AND NOT MD_FORCE_SWITCH_DISPATCH)
                target_compile_definitions(microdos_dos2_e2e_router_${variant} PRIVATE MD_THREADED_DISPATCH=1)
            endif()
            if(MSVC)
                target_compile_definitions(microdos_dos2_e2e_router_${variant} PRIVATE _CRT_SECURE_NO_WARNINGS)
            endif()
            add_dependencies(microdos_dos2_e2e_router_${variant} md_generated)
            add_test(NAME dos2_e2e_router_${variant} COMMAND microdos_dos2_e2e_router_${variant}
                "${MD_MSDOS2_BIN}/MSDOS.SYS" "${MD_E2E_IMG}" --no-cache)
            set_tests_properties(dos2_e2e_router_${variant} PROPERTIES FIXTURES_REQUIRED dos2_image)
        endforeach()
        set_tests_properties(dos2_e2e_aot dos2_e2e_interp dos2_e2e_nocache dos2_e2e_lockstep dos2_e2e_g128
                             PROPERTIES FIXTURES_REQUIRED dos2_image)
    else()
        message(STATUS "microDOS: DOS 2 end-to-end tests skipped (run .\\md.bat deps msdos)")
    endif()
endif()

# M21.1b: generated sources have exactly one owner. Several targets compile
# the same generated files; with a custom command attached to each of them,
# a parallel build could run dosrecomp for one target while another compiled
# the half-written file (seen as a truncated dos2test_recomp.c). Every
# consumer now waits for this single target instead.
get_property(md_generated_outputs GLOBAL PROPERTY MD_GENERATED_OUTPUTS)
add_custom_target(md_generated DEPENDS ${md_generated_outputs})
foreach(md_consumer microdos_bench microdos_dos2_e2e microdos_dos2_e2e_native3 microdos_dos2_e2e_g128 microdos_host
        microdos_msdos2 microdos_tests microdos_region_aot_tests microdos_region_tests
        microdos_jit_diff microdos_jit_m203 microdos_store_tracking)
    if(TARGET ${md_consumer})
        add_dependencies(${md_consumer} md_generated)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Native-3 omnibus host validation / cache tools (N3.0-N3.8)
# ---------------------------------------------------------------------------
add_executable(native3_cache_tool tools/native3_cache.c)
target_include_directories(native3_cache_tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")

if(MD_BUILD_TESTS)
    add_executable(microdos_native3_tests
        tests/test_native3.c
        src/runtime/native3.c
        src/runtime/native_v2.c
        src/runtime/native_v2_runtime.c
        src/runtime/jit_core.c)
    target_link_libraries(microdos_native3_tests PRIVATE microdos::runtime microdos::decode)
    target_include_directories(microdos_native3_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/src/runtime")
    target_compile_definitions(microdos_native3_tests PRIVATE
        MD_N3_PROFILE=1
        MD_JIT_PROFILE=1
        MD_JIT_LEGACY_HOTNESS=0)
    add_test(NAME native3 COMMAND microdos_native3_tests)
endif()

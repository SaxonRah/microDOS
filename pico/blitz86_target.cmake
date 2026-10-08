# Optional sidecar target; included at the END of microDOS/pico/CMakeLists.txt.
# Existing microDOS targets are untouched.
if(NOT DEFINED BLITZ86_ROOT)
    set(BLITZ86_ROOT "C:/blitz86_v2" CACHE PATH "External blitz86 source tree")
endif()
if(NOT EXISTS "${BLITZ86_ROOT}/include/b86.h" OR
   NOT EXISTS "${BLITZ86_ROOT}/src/be_t2.c")
    message(STATUS "blitz86 sidecar: no source at ${BLITZ86_ROOT}; target disabled")
else()
    add_executable(blitz86_pico_probe
        "${MD_ROOT}/pico/blitz86_pico_probe.c"
        "${BLITZ86_ROOT}/src/interp.c"
        "${BLITZ86_ROOT}/src/jit.c"
        "${BLITZ86_ROOT}/src/be_t2.c")
    target_include_directories(blitz86_pico_probe PRIVATE "${BLITZ86_ROOT}/include")
    md_pico_common(blitz86_pico_probe 0)
    target_compile_definitions(blitz86_pico_probe PRIVATE
        MICRODOS_PICO_SYS_KHZ=300000)
    pico_set_program_name(blitz86_pico_probe "blitz86 B1/B2 probe")
    message(STATUS "blitz86 sidecar enabled using ${BLITZ86_ROOT}")
endif()

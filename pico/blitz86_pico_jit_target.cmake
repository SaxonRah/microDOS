# blitz86 RP2350 native-entry JIT probe. Independent of the interpreter probe.
# Scope allocator substitutions to blitz86's JIT translation unit ONLY.
# Never define calloc/free for the Pico SDK's C/C++/assembler sources.
if(EXISTS "${BLITZ86_ROOT}/include/b86.h")
  add_library(blitz86_pico_jit_core OBJECT
    "${BLITZ86_ROOT}/src/jit.c")
  target_include_directories(blitz86_pico_jit_core PRIVATE
    "${BLITZ86_ROOT}/include" "${MD_ROOT}/pico")
  # Table sizes must be identical for jit.c and be_t2.c (lookup stub hash).
  set(B86_PICO_TABLES B86_FAST_BITS=10 B86_MAXB=2048 B86_MAP_BITS=12)
  target_compile_definitions(blitz86_pico_jit_core PRIVATE
    ${B86_PICO_TABLES}
    B86_HW_PICO=1
    calloc=b86_hw_calloc
    free=b86_hw_free)
  target_compile_options(blitz86_pico_jit_core PRIVATE
    "-include" "${MD_ROOT}/pico/b86_hw_alloc.h")

  add_executable(blitz86_pico_jit
    "${MD_ROOT}/pico/blitz86_pico_jit.c"
    "${MD_ROOT}/pico/b86_hw_alloc.c"
    "${BLITZ86_ROOT}/src/interp.c"
    "${BLITZ86_ROOT}/src/be_t2.c")
  target_include_directories(blitz86_pico_jit PRIVATE
    "${BLITZ86_ROOT}/include" "${MD_ROOT}/pico")
  target_link_libraries(blitz86_pico_jit PRIVATE blitz86_pico_jit_core)
  md_pico_common(blitz86_pico_jit 0)
  target_compile_definitions(blitz86_pico_jit PRIVATE B86_HW_PICO=1 ${B86_PICO_TABLES})
  pico_set_program_name(blitz86_pico_jit "blitz86 Thumb-2 JIT native-entry probe")
  message(STATUS "blitz86 Pico native JIT target enabled (isolated JIT allocator)")
endif()

if(EXISTS "${BLITZ86_ROOT}/include/b86.h")
  add_executable(blitz86_pi_jit.elf
    "${MD_ROOT}/pi0w/start.S"
    "${MD_ROOT}/pi0w/mini_libc.c"
    "${MD_ROOT}/pi0w/blitz86_pi_jit.c"
    "${MD_ROOT}/pi0w/b86_hw_alloc.c"
    "${BLITZ86_ROOT}/src/interp.c"
    "${BLITZ86_ROOT}/src/jit.c"
    "${BLITZ86_ROOT}/src/be_a64.c")
  target_include_directories(blitz86_pi_jit.elf PRIVATE
    "${BLITZ86_ROOT}/include" "${MD_ROOT}/pi0w")
  target_compile_options(blitz86_pi_jit.elf PRIVATE
    -O2 -Wall -Wextra -ffunction-sections -fdata-sections
    "$<$<COMPILE_LANGUAGE:C>:-include>"
    "$<$<COMPILE_LANGUAGE:C>:${MD_ROOT}/pi0w/b86_hw_alloc.h>")
  target_compile_definitions(blitz86_pi_jit.elf PRIVATE
    calloc=b86_hw_calloc free=b86_hw_free)
  target_link_options(blitz86_pi_jit.elf PRIVATE
    -nostdlib -static "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
    -Wl,--gc-sections -Wl,--build-id=none
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/blitz86_pi_jit.map")
  target_link_libraries(blitz86_pi_jit.elf PRIVATE gcc)
  add_custom_command(TARGET blitz86_pi_jit.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:blitz86_pi_jit.elf>"
      "${CMAKE_CURRENT_BINARY_DIR}/kernel8_blitz86_jit.img"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:blitz86_pi_jit.elf>"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/kernel8_blitz86_jit.img")
  message(STATUS "blitz86 Pi native JIT target enabled")
endif()

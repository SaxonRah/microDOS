# Add as final include in microDOS/pi0w/CMakeLists.txt.
if(NOT DEFINED BLITZ86_ROOT)
  set(BLITZ86_ROOT "C:/blitz86_v2" CACHE PATH "blitz86 source")
endif()
if(EXISTS "${BLITZ86_ROOT}/include/b86.h")
  add_executable(blitz86_pi_probe.elf
    "${MD_ROOT}/pi0w/start.S"
    "${MD_ROOT}/pi0w/mini_libc.c"
    "${MD_ROOT}/pi0w/blitz86_pi_probe.c"
    "${BLITZ86_ROOT}/src/interp.c")
  # Freestanding reference CPU smoke first. be_a64.c/jit.c need an allocator
  # and executable arena before they can safely be linked and executed.
  target_include_directories(blitz86_pi_probe.elf PRIVATE "${BLITZ86_ROOT}/include")
  target_compile_options(blitz86_pi_probe.elf PRIVATE -O2 -Wall -Wextra -ffunction-sections -fdata-sections)
  target_link_options(blitz86_pi_probe.elf PRIVATE
    -nostdlib -static "-T${CMAKE_CURRENT_LIST_DIR}/linker.ld"
    -Wl,--gc-sections -Wl,--build-id=none
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/blitz86_pi_probe.map")
  target_link_libraries(blitz86_pi_probe.elf PRIVATE gcc)
  add_custom_command(TARGET blitz86_pi_probe.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:blitz86_pi_probe.elf>"
      "${CMAKE_CURRENT_BINARY_DIR}/kernel8_blitz86_probe.img"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:blitz86_pi_probe.elf>"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/kernel8_blitz86_probe.img")
  message(STATUS "blitz86 Pi sidecar enabled from ${BLITZ86_ROOT}")
else()
  message(STATUS "blitz86 Pi sidecar disabled (source missing)")
endif()

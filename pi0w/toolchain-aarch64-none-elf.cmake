set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
set(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)
set(CMAKE_OBJCOPY aarch64-none-elf-objcopy)
set(CMAKE_SIZE aarch64-none-elf-size)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mgeneral-regs-only")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-a53 -ffreestanding")

#include "microdos/runtime.h"

static const char kHello[] = "Hello from microDOS!\r\n$";

MdStopReason md_recomp_hello(MdRuntime *runtime, uint16_t segment)
{
    MdX86 *cpu = &runtime->cpu;
    unsigned i;
    const uint16_t message_offset = 0x010Cu;

    md_runtime_reset(runtime);
    cpu->cs = segment;
    cpu->ds = segment;
    cpu->es = segment;
    cpu->ss = segment;
    cpu->ip = 0x0100u;
    cpu->r[MD_X86_SP] = 0xFFFEu;

    for (i = 0; i < sizeof(kHello); ++i) {
        md_x86_write8(cpu, segment, (uint16_t)(message_offset + i), (uint8_t)kHello[i]);
    }

    /* Generated-code shape for:
         mov ah,09h
         mov dx,message
         int 21h
         mov ax,4c00h
         int 21h
       Each original instruction contributes to the same architectural state used
       by the interpreter. The real recompiler will emit this mechanically. */
    md_x86_set_reg8(cpu, 4u, 0x09u);
    ++runtime->instructions;
    cpu->r[MD_X86_DX] = message_offset;
    ++runtime->instructions;
    ++runtime->instructions;
    (void)md_runtime_interrupt(runtime, 0x21u);
    if (runtime->stop_reason != MD_STOP_NONE) return runtime->stop_reason;

    cpu->r[MD_X86_AX] = 0x4C00u;
    ++runtime->instructions;
    ++runtime->instructions;
    (void)md_runtime_interrupt(runtime, 0x21u);
    return runtime->stop_reason;
}

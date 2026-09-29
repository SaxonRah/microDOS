#ifndef MICRODOS_DECODE_H
#define MICRODOS_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdDecodeFlow {
    MD_DECODE_FLOW_FALLTHROUGH = 0,
    MD_DECODE_FLOW_CONDITIONAL,
    MD_DECODE_FLOW_CALL,
    MD_DECODE_FLOW_JUMP,
    MD_DECODE_FLOW_INDIRECT_CALL,
    MD_DECODE_FLOW_INDIRECT_JUMP,
    MD_DECODE_FLOW_RETURN,
    MD_DECODE_FLOW_STOP
} MdDecodeFlow;

typedef struct MdDecodedInstruction {
    uint16_t ip;
    uint16_t next_ip;
    uint16_t target;
    uint8_t opcode;
    uint8_t length;
    uint8_t prefix_count;
    uint8_t prefixes[8];
    uint8_t has_modrm;
    uint8_t modrm;
    uint8_t valid_8086;
    uint8_t far_control;
    MdDecodeFlow flow;
} MdDecodedInstruction;

/* Structural 16-bit decoder. It determines instruction boundaries and direct
   control-flow targets for the complete 8086 opcode map. It intentionally does
   not execute the instruction. image_base is the guest offset corresponding to
   image[0]. Images may not wrap through 0000h. */
bool md_decode_8086(const uint8_t *image,
                    size_t image_size,
                    uint16_t image_base,
                    uint16_t ip,
                    MdDecodedInstruction *out);

const char *md_decode_flow_name(MdDecodeFlow flow);

/* Coverage helpers describing the execution engines implemented by the current
   microDOS runtime/recompiler. A prefix currently makes an instruction a
   fallback even when its underlying opcode is otherwise implemented. */
bool md_decode_interp_supported(const MdDecodedInstruction *inst);
bool md_decode_aot_supported(const MdDecodedInstruction *inst);

#ifdef __cplusplus
}
#endif

#endif

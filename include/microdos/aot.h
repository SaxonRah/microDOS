#ifndef MICRODOS_AOT_H
#define MICRODOS_AOT_H

#include <stdbool.h>
#include <stdint.h>

#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Descriptor exported by every dosrecomp-generated image so a host can run
   the compiled code for a program that DOS itself loaded (attach mode).

   attach()   verifies the image bytes at segment:0100h, marks them executable
              and arms a byte-exact write guard. Returns false on mismatch.
   ready()    true while that segment's copy is attached and unmodified.
   is_entry() true for IPs where compiled code may be entered.
   enter()    runs compiled code from the current CS:IP until control leaves
              compiled code (INT into DOS, hole, unknown target, invalidation)
              or `budget` instructions retire. Returns MD_STOP_NONE in those
              cases; any other value is a real stop (HALT, FAULT...). */
typedef struct MdAotProgram {
    const char *name;
    uint32_t image_size;
    uint32_t compiled_instructions;
    uint32_t hole_instructions;
    uint32_t entry_count;
    bool (*attach)(MdRuntime *runtime, uint16_t segment);
    bool (*ready)(const MdRuntime *runtime, uint16_t segment);
    bool (*is_entry)(uint16_t ip);
    MdStopReason (*enter)(MdRuntime *runtime, uint64_t budget);
} MdAotProgram;

#ifdef __cplusplus
}
#endif

#endif

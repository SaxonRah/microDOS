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
   is_entry() true for IPs where compiled code may be entered (static).
   block_ok() true when compiled code at segment:ip may run right now: the
              copy is attached and every 64-byte chunk its block spans is
              still unmodified (M17). Hosts use this, not is_entry(), to
              decide whether entering can make progress.
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
    bool (*block_ok)(const MdRuntime *runtime, uint16_t segment, uint16_t ip);
    MdStopReason (*enter)(MdRuntime *runtime, uint64_t budget);
} MdAotProgram;

#ifdef __cplusplus
}
#endif

#endif

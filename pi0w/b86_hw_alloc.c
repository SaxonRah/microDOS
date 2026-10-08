#include "b86_hw_alloc.h"
#include <stdint.h>
#include <string.h>
#ifndef B86_HW_HEAP_BYTES
#define B86_HW_HEAP_BYTES (2u*1024u*1024u)
#endif
#ifdef B86_HW_PICO
#include "hardware/psram.h"
static uint8_t __uninitialized_psram("b86_jit_heap") __attribute__((aligned(64))) heap[B86_HW_HEAP_BYTES];
#else
static uint8_t __attribute__((aligned(64))) heap[B86_HW_HEAP_BYTES];
#endif
static size_t used;
void b86_hw_alloc_reset(void){used=0;}
void *b86_hw_calloc(size_t n,size_t sz){
 if(sz && n > (size_t)-1/sz)return 0;
 size_t bytes=n*sz;
 size_t off=(used+15u)&~(size_t)15u;
 if(off>sizeof(heap)||bytes>sizeof(heap)-off)return 0;
 void *p=heap+off;
 used=off+bytes;
 memset(p,0,bytes);
 return p;
}
void b86_hw_free(void *p){(void)p;}

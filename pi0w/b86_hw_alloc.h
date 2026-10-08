#ifndef B86_HW_ALLOC_H
#define B86_HW_ALLOC_H
#include <stddef.h>
void *b86_hw_calloc(size_t n, size_t sz);
void b86_hw_free(void *p);
void b86_hw_alloc_reset(void);
#endif

#ifndef MICRODOS_HOST_DOS_H
#define MICRODOS_HOST_DOS_H

#include <stddef.h>

#include "microdos/runtime.h"

typedef struct MdHostDos {
    char *output;
    size_t capacity;
    size_t length;
} MdHostDos;

void md_host_dos_init(MdHostDos *host, char *output, size_t capacity);
bool md_host_dos_interrupt(MdRuntime *runtime, uint8_t vector, void *user);

#endif

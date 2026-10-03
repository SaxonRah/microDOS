#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int value, size_t count)
{
    unsigned char *p = (unsigned char *)dst;
    while (count-- != 0u) *p++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (count-- != 0u) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t count)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || count == 0u) return dst;

    if (d < s) {
        while (count-- != 0u) *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count-- != 0u) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *lhs, const void *rhs, size_t count)
{
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;
    while (count-- != 0u) {
        if (*a != *b) return (int)*a - (int)*b;
        ++a;
        ++b;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0') ++p;
    return (size_t)(p - s);
}

#include <stddef.h>

#if defined(__GNUC__)
#define NES_SHIM_NO_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define NES_SHIM_NO_PATTERN
#endif

NES_SHIM_NO_PATTERN void *memcpy(void *dst, const void *src, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (len-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

NES_SHIM_NO_PATTERN void *memset(void *dst, int value, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    while (len-- > 0) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

NES_SHIM_NO_PATTERN void *memmove(void *dst, const void *src, size_t len)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        while (len-- > 0) {
            *d++ = *s++;
        }
    } else {
        d += len;
        s += len;
        while (len-- > 0) {
            *--d = *--s;
        }
    }
    return dst;
}

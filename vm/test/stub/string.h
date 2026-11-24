/* Minimal <string.h> for the freestanding check32 build. */
#ifndef GOGO_STUB_STRING_H
#define GOGO_STUB_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
size_t strlen(const char *);
#endif

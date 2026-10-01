#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* Build the heap on top of the PMM: one contiguous 2 MiB region, carved into
 * 16-byte-aligned blocks with boundary tags.  Must run after pmm_init(). */
void heap_init(void);

void *kmalloc(u32 size);
void  kfree(void *p);

/* Diagnostics: bytes sitting in free chunks, and the largest one. */
u32 heap_free_bytes(void);
u32 heap_largest_free(void);
u32 heap_total_bytes(void);

#endif /* HEAP_H */

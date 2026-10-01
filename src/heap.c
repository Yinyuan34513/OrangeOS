/* ===========================================================================
 * Orange OS -- the kernel heap.
 *
 * A byte-granularity allocator on top of the PMM, which only deals in whole
 * frames.  One contiguous 2 MiB region is taken from the PMM at init and
 * carved into blocks the classic Knuth way:
 *
 *   - every block carries a header { magic, size, next } and is 16-byte
 *     aligned, so payloads are too;
 *   - free blocks are kept on a singly-linked list sorted by address, which
 *     makes freeing a matter of inserting and then merging with the physical
 *     neighbours -- the next block is always at payload_end, no search;
 *   - kmalloc is first-fit, splitting the tail of any block that has room
 *     for a header and 16 more bytes.
 *
 * The whole heap is identity mapped (paging_init maps all of RAM), so a
 * physical address is a usable pointer and no page-table work is needed.
 * =========================================================================== */

#include "types.h"
#include "mem.h"
#include "heap.h"
#include "kputs.h"

#define HEAP_MAGIC_USED 0x1EA7C0DEu
#define HEAP_MAGIC_FREE 0xFEE1BEEFu
#define HEAP_BYTES      (2u << 20)          /* 2 MiB */
#define HEADER          16u                 /* header padded to the alignment */
#define ALIGN           16u

struct chunk {
    u32 magic;
    u32 size;                 /* payload bytes, multiple of ALIGN */
    struct chunk *next;       /* free list, sorted by address; only when free */
};

static struct chunk *free_head;
static u32 heap_lo, heap_hi;  /* region bounds, for kfree sanity checks */

/* ---- diagnostics --------------------------------------------------------- */

u32 heap_free_bytes(void)
{
    u32 sum = 0;
    for (struct chunk *c = free_head; c; c = c->next)
        sum += c->size;
    return sum;
}

u32 heap_largest_free(void)
{
    u32 best = 0;
    for (struct chunk *c = free_head; c; c = c->next)
        if (c->size > best)
            best = c->size;
    return best;
}

u32 heap_total_bytes(void)
{
    return heap_hi - heap_lo;
}

/* ---- init ---------------------------------------------------------------- */

void heap_init(void)
{
    u32 frames = HEAP_BYTES / PAGE_SIZE;
    u32 phys = pmm_alloc_contiguous(frames);

    if (!phys) {
        kputs("heap: no contiguous region, kmalloc is offline\r\n");
        return;
    }

    heap_lo = phys;
    heap_hi = phys + HEAP_BYTES;

    struct chunk *c = (struct chunk *)phys;
    c->magic = HEAP_MAGIC_FREE;
    c->size  = HEAP_BYTES - HEADER;
    c->next  = 0;
    free_head = c;

    /* Prove it before anything depends on it: three allocations, marker
     * written and read back, then freed in order.  Adjacent blocks merge as
     * they are released, so the heap must end up exactly as it started --
     * one free chunk spanning the whole region. */
    u32 want_free = HEAP_BYTES - HEADER;
    u8  *a = kmalloc(100), *b = kmalloc(200), *d = kmalloc(4096);
    int ok = a && b && d
             && ((u32)a & 15) == 0 && ((u32)b & 15) == 0
             && (u8 *)b >= a + 100 && (u8 *)d >= b + 200;

    for (u32 i = 0; ok && i < 100; i++)  a[i] = (u8)(i * 7);
    for (u32 i = 0; ok && i < 200; i++)  b[i] = (u8)(i * 13);
    for (u32 i = 0; ok && i < 100; i++)  ok = a[i] == (u8)(i * 7);
    for (u32 i = 0; ok && i < 200; i++)  ok = b[i] == (u8)(i * 13);

    kfree(a);
    kfree(b);
    kfree(d);

    ok = ok && heap_free_bytes() == want_free
             && heap_largest_free() == want_free;

    kputs("heap: 2 MiB at 0x");
    kputhex(phys);
    kputs(", selftest ");
    kputs(ok ? "PASS" : "FAIL");
    kputs("\r\n");
}

/* ---- allocate ------------------------------------------------------------ */

void *kmalloc(u32 size)
{
    if (!heap_lo)
        return 0;
    if (size == 0)
        size = 1;

    /* payload plus header, rounded so the next block stays 16-aligned */
    u32 need = (size + HEADER + ALIGN - 1) & ~(ALIGN - 1);

    for (struct chunk **link = &free_head; *link; link = &(*link)->next) {
        struct chunk *c = *link;

        if (c->size < need)
            continue;

        if (c->size >= need + HEADER + ALIGN) {
            /* split: keep the front, return the tail to the free list */
            struct chunk *rest = (struct chunk *)((u8 *)c + HEADER + need);
            rest->magic = HEAP_MAGIC_FREE;
            rest->size  = c->size - need - HEADER;
            rest->next  = c->next;
            *link = rest;
            c->size = need;
        } else {
            *link = c->next;        /* close enough: hand out the whole block */
        }

        c->magic = HEAP_MAGIC_USED;
        c->next  = 0;
        return (u8 *)c + HEADER;
    }

    kputs("heap: out of memory (");
    kputdec(size);
    kputs(" bytes)\r\n");
    return 0;
}

/* ---- free ---------------------------------------------------------------- */

void kfree(void *p)
{
    if (!p)
        return;

    u32 addr = (u32)p;
    if (addr < heap_lo + HEADER || addr >= heap_hi) {
        kputs("heap: kfree outside the heap\r\n");
        return;
    }

    struct chunk *c = (struct chunk *)(addr - HEADER);
    if (c->magic != HEAP_MAGIC_USED) {
        kputs("heap: kfree on a block that is not allocated\r\n");
        return;
    }

    /* Insert address-sorted, then merge with the physical neighbours.  The
     * next block is always exactly at payload_end, so both merges are
     * pointer arithmetic, not searches. */
    struct chunk *prev = 0;
    struct chunk **link = &free_head;
    while (*link && *link < c) {
        prev = *link;
        link = &(*link)->next;
    }

    c->magic = HEAP_MAGIC_FREE;
    c->next  = *link;
    *link    = c;

    if (c->next && (u8 *)c + HEADER + c->size == (u8 *)c->next) {
        c->size += HEADER + c->next->size;
        c->next  = c->next->next;
    }
    if (prev && (u8 *)prev + HEADER + prev->size == (u8 *)c) {
        prev->size += HEADER + c->size;
        prev->next  = c->next;
    }
}

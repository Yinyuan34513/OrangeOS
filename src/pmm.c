/* ===========================================================================
 * Orange OS -- the physical memory allocator.
 *
 * boot.asm collected an e820 map with the BIOS while still in real mode and
 * parked it at 0x6000.  From that, this builds a free list of 4K frames --
 * and since a free frame is itself 4K of perfectly good storage, the list
 * lives inside the frames it links.  No metadata, no bitmap, and exactly one
 * word of overhead per frame, which is the frame.
 *
 * Everything below 1 MiB is excluded outright: that is where the BIOS data,
 * our own boot sectors, the payload and its IDT live, and none of it is the
 * allocator's to give away.
 * =========================================================================== */

#include "types.h"
#include "mem.h"

struct e820_entry {
    u64 base;
    u64 len;
    u32 type;      /* 1 = usable RAM */
    u32 acpi;
};

#define E820_COUNT   (*(volatile u32 *)0x6000)
#define E820_ENTRY(i) ((volatile struct e820_entry *)(0x6010 + (i) * 24))
#define E820_USABLE  1
#define MAX_ENTRIES  30

static u32 free_head;         /* physical address of the first free frame */
static u32 free_count, total_count;
static u32 top_addr;

#include "kputs.h"

u32 pmm_total_frames(void) { return total_count; }
u32 pmm_free_frames(void)  { return free_count; }
u32 pmm_top(void)          { return top_addr; }

/* Compat writes NULL as the frame's last word, which is fine: it is the
 * end-of-list marker, and the frame is only valid memory until handed out. */
static void add_range(u64 base, u64 len)
{
    if (base > 0x100000000ULL)
        return;                                   /* beyond 32 bits entirely */

    u32 start = (u32)((base + 0xFFF) & ~0xFFFull);
    u32 end   = (u32)((base + len) & ~0xFFFull);
    if ((base + len) > 0x100000000ULL)
        end = 0xFFFFF000u;
    if (start < 0x100000)
        start = 0x100000;                         /* ours and the BIOS's */
    if (end <= start)
        return;

    /* Link top-down so the head ends up at the lowest address, which makes
     * allocations come out of memory in a predictable order. */
    for (u32 a = end - 0x1000;; a -= 0x1000) {
        *(u32 *)a = free_head;
        free_head = a;
        free_count++;
        total_count++;
        if (a == start)
            break;
    }
}

void pmm_init(void)
{
    u32 n = E820_COUNT;
    if (n > MAX_ENTRIES)
        n = MAX_ENTRIES;

    for (u32 i = 0; i < n; i++) {
        volatile struct e820_entry *e = E820_ENTRY(i);
        if (e->type == E820_USABLE) {
            if (e->base + e->len > top_addr)
                top_addr = (u32)(e->base + e->len);
            add_range(e->base, e->len);
        }
    }

    /* Prove the allocator before anyone depends on it: eight allocations
     * must be distinct, page aligned and nonzero, and freeing them all must
     * restore the count.  Eight frames are handed back in reverse order, so
     * the head moves -- the count is the invariant. */
    u32 saved_free = free_count;
    u32 taken[8];
    int ok = 1;

    for (int i = 0; i < 8; i++) {
        taken[i] = pmm_alloc();
        if (taken[i] == 0 || (taken[i] & 0xFFF))
            ok = 0;
        for (int j = 0; j < i; j++)
            if (taken[i] == taken[j])
                ok = 0;
    }
    for (int i = 0; i < 8; i++)
        pmm_free(taken[i]);

    kputs("\npmm: ");
    kputdec(total_count);
    kputs(" frames from e820, selftest ");
    kputs(ok && free_count == saved_free ? "PASS" : "FAIL");
    kputs("\n");
}

u32 pmm_alloc(void)
{
    if (!free_head)
        return 0;
    u32 p = free_head;
    free_head = *(u32 *)p;
    free_count--;
    return p;
}

/* Demand paging hands frames to the #PF handler, which must not hand the
 * faulting code a page full of the free list's old link pointers. */
u32 pmm_alloc_zeroed(void)
{
    u32 p = pmm_alloc();
    if (p)
        memset((void *)p, 0, PAGE_SIZE);
    return p;
}

void pmm_free(u32 phys)
{
    *(u32 *)phys = free_head;
    free_head = phys;
    free_count++;
}

/* Pull n consecutive frames off the free list.  The list is address-sorted
 * (each region is linked ascending), so a run is "next == prev + PAGE_SIZE"
 * repeated n-1 times; when one is found the whole run is unlinked.  The heap
 * needs this -- its blocks walk linear memory.  Returns the physical base,
 * or 0. */
u32 pmm_alloc_contiguous(u32 n)
{
    u32 prev = 0;        /* node before cur (0 = list head) */
    u32 cur  = free_head;
    u32 before_run = 0;  /* node before run_first -- frozen at run start */
    u32 run_first = 0, run_last = 0, run_len = 0;

    while (cur) {
        u32 next = *(u32 *)cur;

        if (run_len && cur == run_last + PAGE_SIZE) {
            run_last = cur;
            run_len++;
        } else {
            before_run = prev;   /* the node ahead of the (re)started run */
            run_first = cur;
            run_last  = cur;
            run_len   = 1;
        }
        if (run_len == n) {
            /* Skip the WHOLE run.  The first version updated prev per node
             * and then wrote through it, which unlinked only the last frame
             * and left the rest of the run reachable -- the heap then wrote
             * its chunk magic over a list node. */
            if (before_run)
                *(u32 *)before_run = next;
            else
                free_head = next;
            free_count -= n;
            return run_first;
        }
        prev = cur;
        cur  = next;
    }
    return 0;
}

/* ---------------------------------------------------------------- helpers */

void *memset(void *dst, int c, unsigned n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

/* ===========================================================================
 * Orange OS -- i386 paging.
 *
 * Two-level paging, 4K pages, no PSE: CR3 points at a page directory built
 * from frames the PMM handed out.  Two things get mapped:
 *
 *   - every usable byte of RAM, identity (virtual == physical), so nothing
 *     about the running image has to move or be relocated when PG flips on;
 *   - the framebuffer through a separate window at 0xF0000000, which is the
 *     first translation the machine ever performs that is *not* an identity
 *     one: if the screen paints correctly afterwards, the page tables work.
 *
 * The identity map is built by walking every page below pmm_top() through
 * map_page().  That is a few thousand calls, all trivial, and it keeps one
 * code path for "make this virtual address mean that physical one".
 * =========================================================================== */

#include "types.h"
#include "mem.h"

u32 fb_virt = 0;

static u32 *pdir;      /* page directory; identity mapped, so VA == PA */

/* ---- the one way to teach the page tables anything ---------------------- */

static u32 *ensure_table(u32 vaddr)
{
    u32 pde = vaddr >> 22;

    if (!(pdir[pde] & 1)) {                 /* not present */
        u32 table = pmm_alloc();
        if (!table)
            return 0;
        memset((void *)(unsigned long)table, 0, 4096);
        pdir[pde] = table | 0x003;          /* present, read/write */
    }
    return (u32 *)(pdir[pde] & ~0xFFFull);
}

static int map_page(u32 vaddr, u32 phys, u32 extra)
{
    u32 *table = ensure_table(vaddr);
    if (!table)
        return -1;
    table[(vaddr >> 12) & 0x3FF] = (phys & ~0xFFFull) | 0x003 | extra;
    return 0;
}

/* ---- demand paging ------------------------------------------------------- */

/*
 * A virtual range reserved but deliberately NOT mapped: the first touch of
 * any page in it takes a #PF, and paging_demand_map() answers that fault by
 * allocating a zeroed frame and wiring the mapping.  One range is enough for
 * the experiment; more would be a table.
 */
static u32 dm_lo, dm_hi;                 /* [lo, hi) reserved, unmapped */
static u32 dm_pages;                     /* frames handed out so far */

void paging_demand_region(u32 base, u32 bytes)
{
    dm_lo = base;
    dm_hi = base + bytes;
}

/* Called from the page-fault handler.  Returns 0 when the faulting address
 * belongs to the demand region and has been mapped; -1 otherwise. */
int paging_demand_map(u32 vaddr)
{
    if (vaddr < dm_lo || vaddr >= dm_hi)
        return -1;
    u32 phys = pmm_alloc_zeroed();
    if (!phys)
        return -1;
    if (map_page(vaddr & ~0xFFFu, phys, 0) != 0)
        return -1;
    dm_pages++;
    return 0;
}

u32 paging_demand_pages(void)
{
    return dm_pages;
}

/* ---- bring-up ------------------------------------------------------------ */

void paging_init(u32 fb_phys, u32 fb_bytes)
{
    pdir = (u32 *)pmm_alloc();
    memset(pdir, 0, 4096);

    /* Identity: every page below the top of usable RAM. */
    u32 top = (pmm_top() + 0x3FFFFF) & ~0x3FFFFFu;
    for (u32 page = 0; page < top; page += 0x1000)
        map_page(page, page, 0);

    /* Framebuffer window.  PCD makes it uncacheable: the framebuffer is
     * device memory, and the x86 default write-back caches are the wrong
     * answer for a scanout surface. */
    fb_virt = 0xF0000000u;
    for (u32 off = 0; off < fb_bytes; off += 0x1000)
        map_page(fb_virt + off, fb_phys + off, 0x010 /* PCD */);

    /* CR3 first, then PG.  Everything the instruction pointer will touch is
     * identity mapped, so the enable itself is seamless; the jump is the
     * traditional pipeline flush. */
    __asm__ volatile ("mov %0, %%cr3" :: "r"((u32)pdir));
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;                     /* PG */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile ("jmp 1f\n1:");
}

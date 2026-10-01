#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 0x1000u

/* ---- physical memory allocator (e820-driven free list) ---- */

void pmm_init(void);
u32  pmm_alloc(void);
u32  pmm_alloc_zeroed(void);              /* one 4K frame, 0 when exhausted */
void pmm_free(u32 phys);
u32  pmm_total_frames(void);
u32  pmm_free_frames(void);
u32  pmm_alloc_contiguous(u32 n);   /* n consecutive frames, physical base */
u32  pmm_top(void);
void pmm_debug(void);                /* top of usable RAM, bytes */

/* ---- paging (i386 two-level, identity map + framebuffer window) ---- */

void paging_init(u32 fb_phys, u32 fb_bytes);

/* Reserve [base, base+bytes) of virtual address space without mapping it.
 * The first touch of any page inside takes a #PF, which the handler answers
 * with a zeroed frame (demand paging). */
void paging_demand_region(u32 base, u32 bytes);
int  paging_demand_map(u32 vaddr);       /* called from the #PF handler */
u32  paging_demand_pages(void);
extern u32 fb_virt;                /* where the framebuffer lives once paging is on */

void *memset(void *dst, int c, unsigned n);

#endif /* MEM_H */

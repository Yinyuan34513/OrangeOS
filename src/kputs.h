#ifndef KPUTS_H
#define KPUTS_H

#include "types.h"

/* COM1 writers shared by the early subsystems (pmm, heap, interrupts).
 * The port was programmed by boot.asm at 115200 8N1; everything is polled
 * and works both before and after paging is on (the console lives in the
 * identity map). */
void kputs(const char *s);
void kputhex(u32 v);
void kputdec(u32 v);

#endif /* KPUTS_H */

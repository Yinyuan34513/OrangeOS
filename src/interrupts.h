#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include "types.h"

/* Bring up the IDT, remap the PIC, unmask the timer and keyboard, enable
 * interrupts.  After this returns, hlt is a usable idle instruction and
 * keys arrive in kgetc(). */
void interrupts_init(void);

/* Next key from the PS/2 keyboard, or -1 if none is buffered. */
int kgetc(void);

/* Proofs that the interrupt path is alive: the timer keeps counting with
 * nobody touching anything, and keys counts what the keyboard sent. */
extern volatile u32 timer_ticks;
extern volatile u32 key_count;

/* Page-fault counters: every #PF, and those fixed by demand mapping. */
extern volatile u32 pf_total;
extern volatile u32 pf_demand;

#endif /* INTERRUPTS_H */

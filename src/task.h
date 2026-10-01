#ifndef TASK_H
#define TASK_H

#include "types.h"

/* Adopt the running boot flow as task 0 and turn the timer into a
 * preemptive round-robin scheduler. */
void task_init(void);

/* Create a kernel task running @entry on a fresh stack.  Returns the task
 * id, or -1.  The stack comes from the heap and is never reclaimed
 * (kernel tasks are immortal unless they call task_exit). */
int task_create(void (*entry)(void), const char *name);

/* Mark the current task dead; the next tick switches away and it never
 * runs again.  Never returns. */
void task_exit(void);

/* Print the task table (the `ps` command). */
void task_list(void);

/* Called from the timer IRQ stub with the interrupted esp.  Advances the
 * round-robin and returns the next task's saved esp, or 0 to keep running
 * the current one. */
u32 task_schedule(u32 esp);

#endif /* TASK_H */

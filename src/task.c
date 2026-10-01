/* ===========================================================================
 * Orange OS -- kernel tasks and the preemptive scheduler.
 *
 * The timer IRQ0 is the switch.  Its stub saves the interrupted task's
 * whole register file on its own stack (pusha), hands the esp to
 * task_schedule(), and -- if the scheduler points at a different task --
 * simply moves esp to that task's saved esp before popa + iret.  Nothing
 * else is involved: a task IS a saved stack pointer.
 *
 * A new task is entered the same way from the other side: task_create()
 * forges a stack that looks exactly like an interrupted one (zeroed
 * registers, EIP = entry, CS = kernel, EFLAGS with IF set), so its first
 * slice starts with a "return" into the entry function.
 *
 * The boot flow itself is task 0: its esp is captured on the first tick
 * like any other task's, and the shell continues to run inside its slice.
 * =========================================================================== */

#include "types.h"
#include "task.h"
#include "heap.h"
#include "interrupts.h"
#include "kputs.h"

static void outb(u16 port, u8 v)
{
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

#define TASK_MAX 8

enum { TASK_RUNNABLE, TASK_DEAD };

struct task {
    u32         esp;      /* saved kernel esp, top of the pusha frame */
    u32         slices;   /* time slices consumed */
    const char *name;
    u8          state;
};

static struct task tasks[TASK_MAX];
static int task_count;      /* task 0 = the boot flow, adopted by task_init */
static int task_current;

/* ---- the scheduler, called from the IRQ0 stub --------------------------- */

u32 task_schedule(u32 esp)
{
    timer_ticks++;                 /* the PIT was set to 100 Hz */
    outb(0x20, 0x20);              /* EOI first: switching must not eat IRQs */

    if (task_count < 2)
        return 0;                  /* nothing to switch between yet */

    tasks[task_current].esp    = esp;
    tasks[task_current].slices++;

    do {
        task_current = (task_current + 1) % task_count;
    } while (tasks[task_current].state != TASK_RUNNABLE);

    return tasks[task_current].esp;
}

/* ---- lifecycle ----------------------------------------------------------- */

void task_init(void)
{
    /* The boot flow becomes task 0.  Its esp is not known here -- it is
     * captured by the first timer tick, like every other task's. */
    tasks[0].name   = "shell";
    tasks[0].state  = TASK_RUNNABLE;
    task_current    = 0;
    task_count      = 1;
    kputs("tasks: scheduler on, 100 Hz preemption\r\n");
}

int task_create(void (*entry)(void), const char *name)
{
    if (task_count >= TASK_MAX)
        return -1;

    u8 *stack = kmalloc(4096);
    if (!stack)
        return -1;

    /* Forge the frame the switcher expects, walking down from the top:
     * the iret trio (eflags with IF set, kernel cs, entry) above a full
     * pusha image of zeroed registers.  The first switch "returns" into
     * entry with interrupts enabled.  popa skips the esp slot by itself. */
    u32 *p = (u32 *)(stack + 4096);
    *(--p) = 0x202u;                      /* eflags: IF = 1 */
    *(--p) = 0x08u;                       /* cs: kernel code segment */
    *(--p) = (u32)entry;
    for (int i = 0; i < 8; i++)
        *(--p) = 0;                       /* eax ecx edx ebx esp ebp esi edi */

    struct task *t = &tasks[task_count++];
    t->esp    = (u32)p;
    t->slices = 0;
    t->name   = name;
    t->state  = TASK_RUNNABLE;
    return (int)(t - tasks);
}

void task_exit(void)
{
    tasks[task_current].state = TASK_DEAD;
    kputs("tasks: exit\r\n");
    /* Keep hlt-ing: the next tick switches away and this task is never
     * scheduled again.  Its stack stays allocated. */
    for (;;)
        __asm__ volatile ("hlt");
}

/* ---- ps ------------------------------------------------------------------ */

void task_list(void)
{
    kputs("ps: ");
    kputdec((u32)task_count);
    kputs(" tasks, 100 Hz round-robin\n");
    for (int i = 0; i < task_count; i++) {
        kputs("  ");
        kputdec((u32)i);
        kputs(" ");
        kputs(tasks[i].name);
        kputs(tasks[i].state == TASK_RUNNABLE ? "  RUNNABLE" : "  DEAD     ");
        kputs("  slices ");
        kputdec(tasks[i].slices);
        kputs("\n");
    }
}

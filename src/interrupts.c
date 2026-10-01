/* ===========================================================================
 * Orange OS -- IDT, PIC and the keyboard.
 *
 * The first interrupt controller work of the project.  All 256 IDT gates get
 * a handler (a gate left empty is a triple fault the first time something
 * fires), the PIC is remapped away from the CPU exceptions, and exactly two
 * lines are unmasked: the timer, because it fires whether we like it or not
 * and an un-EOI'd PIC goes silent forever, and the keyboard.
 *
 * Everything still runs with the serial console as the primary I/O; the
 * keyboard feeds the same ring buffer the shell reads from, which is why
 * both can be typed into interchangeably.
 * =========================================================================== */

#include "types.h"
#include "interrupts.h"
#include "kputs.h"
#include "mem.h"

/* ---- ports ---- */
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1
#define KBD_DATA 0x60
#define PIC_EOI 0x20

static u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void outb(u16 port, u8 v)
{
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}

/* ---- IDT ----------------------------------------------------------------- */

struct idt_entry {
    u16 lo;
    u16 sel;
    u8  zero;
    u8  flags;      /* 0x8E: present, ring 0, 32-bit interrupt gate */
    u16 hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtr;

static void idt_set(int n, u32 handler)
{
    idt[n].lo    = handler & 0xFFFF;
    idt[n].sel   = 0x08;               /* the flat code segment boot.asm built */
    idt[n].zero  = 0;
    idt[n].flags = 0x8E;
    idt[n].hi    = handler >> 16;
}

/* ---- handlers ------------------------------------------------------------ */

volatile u32 timer_ticks = 0;
volatile u32 key_count   = 0;

/* CPU exceptions must not be returned from silently -- and several of them
 * push an error code, so a plain iret would land on the wrong bytes anyway.
 * Print the vector and stop. */
static void fault_halt(int n)
{
    kputs("\nOrange OS: CPU exception ");
    kputdec((u32)n);
    kputs(" -- halted\n");
    for (;;)
        __asm__ volatile ("hlt");
}

#define FAULT(n)                                        \
    __attribute__((interrupt)) static void fault_##n(void *frame)   \
    {                                                   \
        (void)frame;                                    \
        fault_halt(n);                                  \
    }

FAULT(0)  FAULT(1)  FAULT(2)  FAULT(3)  FAULT(4)  FAULT(5)  FAULT(6)  FAULT(7)
FAULT(8)  FAULT(9)  FAULT(10) FAULT(11) FAULT(12) FAULT(13)          FAULT(15)
FAULT(16) FAULT(17) FAULT(18) FAULT(19) FAULT(20) FAULT(21) FAULT(22) FAULT(23)
FAULT(24) FAULT(25) FAULT(26) FAULT(27) FAULT(28) FAULT(29) FAULT(30) FAULT(31)

/* Vectors above the exceptions: anything unexplained just returns. */
__attribute__((interrupt)) static void isr_spurious(void *frame)
{
    (void)frame;
}

/*
 * The timer stub doubles as the context switch.  It hands the interrupted
 * esp to task_schedule(); when the scheduler points at another task, esp
 * simply moves to that task's saved frame and the popa + iret below resume
 * THERE instead -- preemptive multitasking in five instructions.
 */
__asm__(
    ".text\n"
    ".global irq0_stub\n"
    "irq0_stub:\n"
    "   pusha\n"
    "   pushl %esp\n"              /* arg: interrupted esp            */
    "   call task_schedule\n"
    "   addl $4, %esp\n"
    "   testl %eax, %eax\n"
    "   je 1f\n"                    /* 0: no switch, keep this stack   */
    "   movl %eax, %esp\n"          /* else: adopt the next task's     */
    "1: popa\n"
    "   iret\n"
);

/* ---- keyboard ------------------------------------------------------------ */

static volatile u8  kbuf[256];
static volatile u32 khead, ktail;
static int shift, caps;

/* Scan code set 1, indexed by make code.  0 = no key. */
static const char keymap[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 10, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};

static const char keymap_shift[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 10, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' '
};

__attribute__((interrupt)) static void irq_kbd(void *frame)
{
    (void)frame;
    u8 sc = inb(KBD_DATA);
    outb(PIC1_CMD, PIC_EOI);

    /* Break codes release a key; only shift matters for those. */
    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36)
            shift = 0;
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift = 1; return; }
    if (sc == 0x3A)               { caps ^= 1; return; }
    if (sc >= sizeof keymap)      { return; }  /* F-keys, arrows, E0 stuff */

    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (!c)
        return;
    if (caps && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');

    if (((khead + 1) & 255) == ktail)
        return;                            /* full: drop it, do not block */
    kbuf[khead] = (u8)c;
    khead = (khead + 1) & 255;
    key_count++;
}

int kgetc(void)
{
    if (khead == ktail)
        return -1;
    int c = kbuf[ktail];
    ktail = (ktail + 1) & 255;
    return c;
}

/* ---- bring-up ------------------------------------------------------------ */

static void pic_remap(void)
{
    /* Cascade the two 8259s onto vectors 0x20..0x2F: below 0x20 belongs to
     * the CPU exceptions, and where the BIOS left them (0x08) is exactly
     * where the divide-by-zero gate lives. */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* Mask everything but the timer and the keyboard. */
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

/* ---- page fault: the recoverable exception ------------------------------- */

volatile u32 pf_total  = 0;   /* every #PF taken, fixed or not */
volatile u32 pf_demand = 0;   /* fixed by mapping a demand page */

/*
 * The #PF stub.  The CPU pushes an error code for vector 14, which iret does
 * not pop, so a C function with the interrupt attribute is the wrong shape
 * here.  This stub builds the argument pair (faulting address from CR2, error
 * code), calls the C logic, and -- if that returns, meaning the fault was
 * fixed -- drops the error code and irets so the faulting instruction runs
 * again, now over a present page.
 */
__asm__(
    ".text\n"
    ".global pf_stub\n"
    "pf_stub:\n"
    "   pusha\n"
    "   movl 32(%esp), %ecx\n"     /* error code  (CPU pushed it)   */
    "   movl 36(%esp), %edx\n"     /* faulting EIP                 */
    "   pushl %edx\n"              /* arg3: rip                    */
    "   pushl %ecx\n"              /* arg2: errcode                */
    "   movl %cr2, %eax\n"         /* faulting linear address      */
    "   pushl %eax\n"              /* arg1: vaddr                  */
    "   call pf_handler_c\n"
    "   addl $12, %esp\n"          /* three arguments              */
    "   popa\n"
    "   addl $4, %esp\n"           /* drop the error code: iret expects EIP */
    "   iret\n"
);

void pf_stub(void);        /* the asm entry, defined just above */
void pf_handler_c(u32 vaddr, u32 errcode, u32 rip);

void pf_handler_c(u32 vaddr, u32 errcode, u32 rip)
{
    pf_total++;

    /* Fixable: the page is simply not there yet (P=0), the access came from
     * ring 0 (U=0), and no reserved bits were involved (RSVD=0) -- and the
     * address belongs to the demand region.  Anything else is a real bug. */
    if (!(errcode & 1) && !(errcode & 4) && !(errcode & 8)
        && paging_demand_map(vaddr) == 0) {
        pf_demand++;
        return;                       /* the stub irets; the instruction re-runs */
    }

    kputs("\nOrange OS: unrecoverable page fault at 0x");
    kputhex(vaddr);
    kputs(" err=0x");
    kputhex(errcode);
    kputs(" rip=0x");
    kputhex(rip);
    kputs(" -- halted\n");
    for (;;)
        __asm__ volatile ("hlt");
}

void task_schedule(u32 esp);           /* src/task.c */
void irq0_stub(void);                  /* the asm entry, defined just above */

void interrupts_init(void)
{
    /* The default divider gives 18.2 Hz -- too coarse to schedule on.  Run
     * the PIT at 100 Hz: ten milliseconds per time slice. */
    outb(0x43, 0x36);                  /* channel 0, mode 3, binary */
    outb(0x40, (1193182 / 100) & 0xFF);
    outb(0x40, (1193182 / 100) >> 8);

    khead = ktail = 0;
    timer_ticks = 0;
    key_count = 0;
    shift = caps = 0;

    for (int i = 0; i < 256; i++)
        idt_set(i, (u32)isr_spurious);

    idt_set(0x20, (u32)irq0_stub);
    idt_set(0x21, (u32)irq_kbd);
    /* The 32 exception gates, individually, because each needs to name its
     * own vector when it fires. */
    idt_set(0,  (u32)fault_0);   idt_set(1,  (u32)fault_1);
    idt_set(2,  (u32)fault_2);   idt_set(3,  (u32)fault_3);
    idt_set(4,  (u32)fault_4);   idt_set(5,  (u32)fault_5);
    idt_set(6,  (u32)fault_6);   idt_set(7,  (u32)fault_7);
    idt_set(8,  (u32)fault_8);   idt_set(9,  (u32)fault_9);
    idt_set(10, (u32)fault_10);  idt_set(11, (u32)fault_11);
    idt_set(12, (u32)fault_12);  idt_set(13, (u32)fault_13);
    idt_set(14, (u32)pf_stub);   idt_set(15, (u32)fault_15);
    idt_set(16, (u32)fault_16);  idt_set(17, (u32)fault_17);
    idt_set(18, (u32)fault_18);  idt_set(19, (u32)fault_19);
    idt_set(20, (u32)fault_20);  idt_set(21, (u32)fault_21);
    idt_set(22, (u32)fault_22);  idt_set(23, (u32)fault_23);
    idt_set(24, (u32)fault_24);  idt_set(25, (u32)fault_25);
    idt_set(26, (u32)fault_26);  idt_set(27, (u32)fault_27);
    idt_set(28, (u32)fault_28);  idt_set(29, (u32)fault_29);
    idt_set(30, (u32)fault_30);  idt_set(31, (u32)fault_31);

    idtr.limit = sizeof idt - 1;
    idtr.base  = (u32)idt;
    __asm__ volatile ("lidt %0" :: "m"(idtr));

    pic_remap();

    /* First sti of the project's life.  From here on hlt wakes up. */
    __asm__ volatile ("sti");
}

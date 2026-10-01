/* ===========================================================================
 * Orange OS / 橙光 OS -- the C payload.
 *
 * Everything after the mode switch lives here.  boot.asm brings the machine
 * up (VBE linear framebuffer, A20, protected mode), parks the boot
 * information at 0x7000 and calls c_start() at 0x10000, where this file is
 * linked.  The wordmark bitmaps come from build/fonts.c, which mkfont.py
 * rasterizes out of the system fonts.
 * =========================================================================== */

#include "types.h"
#include "interrupts.h"
#include "mem.h"
#include "heap.h"
#include "task.h"
#include "kputs.h"

/* boot.asm fills this in before entering protected mode. */
struct bootinfo {
    u32 fb;      /* linear framebuffer physical address */
    u32 w, h;    /* pixels */
    u32 pitch;   /* bytes per scanline */
    u32 bpp;     /* bits per pixel: 24 or 32 */
};

#define BI ((volatile struct bootinfo *)0x7000)

#include "fonts.h"

/* Colours are stored as (red << 16) | (green << 8) | blue.  The framebuffer
 * wants blue in the low byte, which is the same dword written out as-is. */
enum { ORANGE = 0x00F5821F, BLUE = 0x001B6BB5, INK = 0x00263238, WHITE = 0x00FFFFFF };

static u32 psize; /* bytes per pixel: 3 or 4 */

/* ---------------------------------------------------------------- pixels */

static void put_px(u32 x, u32 y, u32 color)
{
    if (x >= BI->w || y >= BI->h)
        return;

    volatile u8 *p = (volatile u8 *)(unsigned long)(fb_virt + y * BI->pitch + x * psize);
    p[0] = color & 0xFF;         /* blue  */
    p[1] = (color >> 8) & 0xFF;  /* green */
    p[2] = (color >> 16) & 0xFF; /* red   */
    if (psize == 4)
        p[3] = 0;
}

static void fill(u32 color)
{
    for (u32 y = 0; y < BI->h; y++)
        for (u32 x = 0; x < BI->w; x++)
            put_px(x, y, color);
}

/* ---------------------------------------------------------------- sprites */

/*
 * The wordmark bitmaps are 1 bit per pixel, MSB first, every row padded to a
 * whole byte.  That padding is the stride: the bit for column c of row r sits
 * at r * stride * 8 + c, NOT r * w + c.  Getting that wrong shears every row
 * sideways by a few pixels, which is exactly what the first attempt did.
 */
static void blit(const struct glyph *g, u32 x0, u32 y0, u32 color)
{
    u32 stride = (g->w + 7) / 8;

    for (u32 row = 0; row < g->h; row++) {
        const u8 *line = g->bits + row * stride;
        for (u32 col = 0; col < g->w; col++)
            if (line[col / 8] & (0x80 >> (col % 8)))
                put_px(x0 + col, y0 + row, color);
    }
}

static void draw_centered(const struct glyph *g, u32 y, u32 color)
{
    blit(g, (BI->w - g->w) / 2, y, color);
}

/* ---------------------------------------------------------------- swoosh */

/*
 * The flick under the wordmark: a quadratic curve drawn as vertical strokes
 * whose thickness grows along t, thin at the tail and fat at the head.
 * Fixed point 16.16-ish: t runs 0..256, so every product stays inside 32
 * bits and the >>16 lands back on pixel coordinates.
 */
static void swoosh(u32 x0, u32 y0, u32 x1, u32 y1, u32 color)
{
    u32 cx = (x0 + x1) / 2, cy = y0 + 78;

    for (u32 t = 0; t <= 256; t++) {
        u32 u = 256 - t;
        u32 x = (x0 * u * u + 2 * cx * u * t + x1 * t * t) >> 16;
        u32 y = (y0 * u * u + 2 * cy * u * t + y1 * t * t) >> 16;
        u32 thick = 2 + t / 32;
        u32 half = thick / 2;

        for (u32 d = 0; d < thick; d++)
            put_px(x, y - half + d, color);
    }
}

/* ---------------------------------------------------------------- layout */

static void paint(void)
{
    fill(WHITE);

    /* the wordmark: "Orange" and "OS" centred as a pair, bottom-aligned so
     * the descender of the g does not lift "OS" off the baseline */
    u32 total = glyph_orange.w + 12 + glyph_os.w;
    u32 tx = (BI->w - total) / 2;
    u32 ty = BI->h / 2 - 140;

    blit(&glyph_orange, tx, ty, ORANGE);
    blit(&glyph_os, tx + glyph_orange.w + 12, ty + glyph_orange.h - glyph_os.h, BLUE);

    swoosh(tx + 30, ty + glyph_orange.h + 34,
           tx + glyph_orange.w + glyph_os.w + 30, ty + glyph_orange.h + 34, BLUE);

    draw_centered(&glyph_slogan, ty + 190, INK);
    draw_centered(&glyph_brand, ty + 250, BLUE);
}

/* ------------------------------------------------------- demo spin tasks */

/* Two tasks that do nothing but count.  Their slice counts in `ps` are the
 * proof of preemption: the numbers grow without anyone typing anything. */
static volatile u32 iter_a, iter_b;

static void demo_a(void) { for (;;) iter_a++; }
static void demo_b(void) { for (;;) iter_b++; }

/* A task that prints a few lines and exits -- created by the spawn command.
 * It waits on ticks (not busy loops) so the other tasks keep their slices. */
static volatile int greeter_running;

static void greeter(void)
{
    for (int i = 1; i <= 3; i++) {
        u32 until = timer_ticks + 50;          /* half a second */
        while ((int)(timer_ticks - until) < 0)
            __asm__ volatile ("hlt");
        kputs("greeter: hello from a kernel task (");
        kputdec((u32)i);
        kputs("/3)\n");
    }
    kputs("greeter: exiting\n");
    greeter_running = 0;
    task_exit();                               /* never returns */
}

/* ---------------------------------------------------------------- serial */

/*
 * Basic I/O over COM1, which boot.asm already programmed to 115200 8N1.
 * Polling only: interrupts stay off because there is no IDT yet, so an
 * interrupt right now would be a triple fault.  That makes `hlt` useless as
 * an idle instruction (with IF=0 it never returns), hence the `pause` in
 * every wait loop.
 */
#define COM1 0x3F8
#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY  0x20

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

static void outw(u16 port, u16 v)
{
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}

static void sputc(char c)
{
    if (c == '\n')
        sputc('\r');
    while (!(inb(COM1 + 5) & LSR_THR_EMPTY))
        __asm__ volatile ("pause");
    outb(COM1, (u8)c);
}

/*
 * One input stream, two sources: the PS/2 keyboard arrives by interrupt,
 * the serial line is still polled.  hlt is safe now -- the IDT is up -- so
 * the idle here sleeps until the next tick instead of burning the CPU.
 */
static int getchar_any(void)
{
    for (;;) {
        int c = kgetc();
        if (c >= 0)
            return c;
        if (inb(COM1 + 5) & LSR_DATA_READY)
            return inb(COM1) & 0xFF;
        __asm__ volatile ("hlt");
    }
}

static void sputs(const char *s)
{
    while (*s)
        sputc(*s++);
}

static void sputs_hex(u32 v);
static void sputs_dec(u32 v);

/*
 * Read one line, echoing as it goes.  Backspace erases on a dumb terminal
 * the only way there is: over-print with a space.  Enter arrives as '\r'.
 */
static int sreadline(char *buf, int max)
{
    int n = 0;

    for (;;) {
        char c = (char)getchar_any();

        if (c == '\r' || c == '\n') {
            sputc('\n');
            buf[n] = 0;
            return n;
        }
        if (c == 0x08 || c == 0x7F) {
            if (n > 0) {
                n--;
                sputs("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < max - 1) {
            buf[n++] = c;
            sputc(c);
        }
    }
}

/* ---------------------------------------------------------------- shell */

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* "echo something" is not a full-string match, so the variable-argument
 * commands get a real prefix test. */
static int prefix(const char *line, const char *p)
{
    while (*p)
        if (*line++ != *p++)
            return 0;
    return 1;
}

static void cmd_help(void)
{
    sputs("commands:\n");
    sputs("  help     this list\n");
    sputs("  echo ..  print its arguments back\n");
    sputs("  sysinfo  framebuffer, CPU, build info\n");
    sputs("  logo     repaint the brand on the screen\n");
    sputs("  clear    clear the serial terminal\n");
    sputs("  heap     kernel heap statistics\n");
    sputs("  kmtest   kernel heap churn test\n");
    sputs("  pfault   demand-paging demo (second run: 0 faults)\n");
    sputs("  ps       task table + demo counters\n");
    sputs("  spawn    create a printing greeter task\n");
    sputs("  poweroff QEMU ACPI poweroff\n");
}

static void cmd_sysinfo(void)
{
    u32 a, b, c, d;
    char vendor[13];

    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    vendor[0] = (u8)(b); vendor[1] = (u8)(b >> 8); vendor[2] = (u8)(b >> 16); vendor[3] = (u8)(b >> 24);
    vendor[4] = (u8)(d); vendor[5] = (u8)(d >> 8); vendor[6] = (u8)(d >> 16); vendor[7] = (u8)(d >> 24);
    vendor[8] = (u8)(c); vendor[9] = (u8)(c >> 8); vendor[10] = (u8)(c >> 16); vendor[11] = (u8)(c >> 24);
    vendor[12] = 0;

    sputs("Orange OS / \xe6\xa9\x99\xe5\x85\x89 OS  (CHENG GUANG OS \xc2\xb7 NUBASY)\n");
    sputs("serial I/O: OK, polled COM1 at 115200 8N1\n");
    sputs("CPU vendor: ");
    sputs(vendor);
    sputc('\n');

    sputs("interrupts: ticks ");
    sputs_dec(timer_ticks);
    sputs(", keys ");
    sputs_dec(key_count);
    sputc('\n');

    sputs("memory: ");
    sputs_dec(pmm_total_frames());
    sputs(" frames (");
    sputs_dec(pmm_total_frames() / 256);
    sputs(" MiB), free ");
    sputs_dec(pmm_free_frames());
    sputc('\n');
    sputs("demand paging: ");
    sputs_dec(pf_demand);
    sputs(" pages mapped, ");
    sputs_dec(pf_total);
    sputs(" faults total\n");
    sputs("paging: on, identity to ");
    sputs_dec(pmm_top() >> 20);
    sputs(" MiB, fb mapped at 0x");
    sputs_hex(fb_virt);
    sputc('\n');

    sputs("framebuffer: ");
    sputs_hex(BI->fb);
    sputs(", ");
    sputs_dec(BI->w);
    sputc('x');
    sputs_dec(BI->h);
    sputs(", ");
    sputs_dec(BI->bpp);
    sputs("bpp, pitch ");
    sputs_dec(BI->pitch);
    sputc('\n');
}

static void shell(void)
{
    static char line[128];

    sputs("\nOrange OS / \xe6\xa9\x99\xe5\x85\x89 OS -- CHENG GUANG OS \xc2\xb7 NUBASY\n");
    sputs("\xe6\xa9\x99\xe5\x85\x89\xe9\x9a\x8f\xe8\xa1\x8c\xef\xbc\x8c\xe6\xb5\x81\xe7\x95\x85\xe6\x97\xa0\xe7\x95\x8c\n");
    sputs("type 'help' for commands\n");

    for (;;) {
        int n;

        sputs("\nOrange OS> ");
        n = sreadline(line, sizeof line);
        (void)n;

        if (!line[0])
            continue;

        if (eq(line, "help")) {
            cmd_help();
        } else if (eq(line, "clear")) {
            sputs("\x1b[2J\x1b[H");
        } else if (eq(line, "logo")) {
            paint();
            sputs("repainted\n");
        } else if (eq(line, "sysinfo")) {
            cmd_sysinfo();
        } else if (eq(line, "poweroff")) {
            /* QEMU's PIIX4 ACPI power-management register: writing the
             * sleep-enable bit with SLP_TYP=0 powers the machine off.  If
             * this is ever run on something without that emulation, say so
             * and carry on. */
            sputs("powering off\n");
            outw(0x604, 0x2000);
            sputs("poweroff not supported here\n");
        } else if (eq(line, "ps")) {
            task_list();
            sputs("demo iters: a=");
            sputs_dec(iter_a);
            sputs(", b=");
            sputs_dec(iter_b);
            sputc('\n');
        } else if (eq(line, "spawn")) {
            if (greeter_running) {
                sputs("spawn: greeter still running\n");
            } else if (task_create(greeter, "greeter") < 0) {
                sputs("spawn: no task slot\n");
            } else {
                greeter_running = 1;
                sputs("spawn: greeter started (prints 3 lines, then exits)\n");
            }
        } else if (eq(line, "pfault")) {
            /* Sparse touches: 4 pages of an 8 MiB reserved region, chosen so
             * they are far apart.  Each first touch takes one #PF, answered
             * with a zeroed frame; a second run must take none. */
            static const u32 idx[] = { 0, 1, 5, 100 };
            u32 f0 = pf_total, d0 = pf_demand, m0 = pmm_free_frames();
            int ok = 1;
            for (unsigned i = 0; i < 4; i++) {
                volatile u32 *page = (volatile u32 *)(0xE0000000u + idx[i] * PAGE_SIZE);
                page[0]   = idx[i] * 2654435761u + 1;
                page[1023] = idx[i] * 40503u + 2;
                ok = ok && page[0] == idx[i] * 2654435761u + 1
                         && page[1023] == idx[i] * 40503u + 2;
            }
            sputs("pfault: faults taken this run: ");
            sputs_dec(pf_total - f0);
            sputs(", pages demand-mapped: ");
            sputs_dec(pf_demand - d0);
            sputs(", pmm frames consumed: ");
            sputs_dec(m0 - pmm_free_frames());
            sputc('\n');
            sputs("pfault: content ");
            sputs(ok ? "verified" : "CORRUPT");
            sputs(" (a second run must report 0 faults)\n");
        } else if (eq(line, "heap")) {
            sputs("heap: total ");
            sputs_dec(heap_total_bytes());
            sputs(" bytes, free ");
            sputs_dec(heap_free_bytes());
            sputs(", largest free ");
            sputs_dec(heap_largest_free());
            sputc('\n');
        } else if (eq(line, "kmtest")) {
            /* churn: allocate a spread of sizes, verify, free in mixed
             * order -- the free count must land exactly back where it was,
             * which is what merges are for. */
            u32 before = heap_free_bytes();
            void *blk[8];
            u32 sizes[8] = { 16, 64, 512, 4096, 33, 2048, 7, 65536 };
            int ok = 1;
            for (int i = 0; i < 8; i++) {
                blk[i] = kmalloc(sizes[i]);
                u8 *q = blk[i];
                for (u32 j = 0; ok && q && j < sizes[i]; j += 61) {
                    q[j] = (u8)(i + j);
                    ok = q[j] == (u8)(i + j);
                }
            }
            kfree(blk[3]); kfree(blk[0]); kfree(blk[6]);
            kfree(blk[7]); kfree(blk[2]); kfree(blk[5]);
            kfree(blk[1]); kfree(blk[4]);
            ok = ok && heap_free_bytes() == before;
            sputs("kmtest: ");
            sputs(ok ? "PASS" : "FAIL");
            sputs(" (free ");
            sputs_dec(heap_free_bytes());
            sputs(" bytes)\n");
        } else if (prefix(line, "echo ")) {
            sputs("> ");
            sputs(line + 5);
            sputc('\n');
        } else {
            sputs("unknown command: ");
            sputs(line);
            sputc('\n');
        }
    }
}

void c_start(void);

__attribute__((section(".text.c_start"))) void c_start(void)
{
    psize = BI->bpp / 8;
    fb_virt = BI->fb;        /* until paging moves it to its own window */
    interrupts_init();       /* IDT up first: a paging bug then gets a diagnosis */
    pmm_init();
    paging_init(BI->fb, BI->pitch * BI->h);
    heap_init();
    paging_demand_region(0xE0000000u, 8u << 20);   /* 8 MiB of lazily-mapped VA */
    task_init();                                   /* boot flow = task 0 */
    task_create(demo_a, "demo-a");                 /* silent spinners: the */
    task_create(demo_b, "demo-b");                 /* scheduler counts them */
    paint();
    shell();
}

static void sputs_hex(u32 v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        sputc(digits[(v >> i) & 0xF]);
}

static void sputs_dec(u32 v)
{
    char buf[11];
    int i = 0;
    do {
        buf[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (i--)
        sputc(buf[i]);
}

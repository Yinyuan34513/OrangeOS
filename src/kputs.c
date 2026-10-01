/* COM1 output shared by the early subsystems.  See kputs.h. */

#include "types.h"
#include "kputs.h"

#define COM1 0x3F8

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

static void kputc(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        __asm__ volatile ("pause");
    outb(COM1, (u8)c);
}

void kputs(const char *s)
{
    while (*s) {
        if (*s == '\n')
            kputc('\r');
        kputc(*s++);
    }
}

void kputhex(u32 v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        kputc(digits[(v >> i) & 0xF]);
}

void kputdec(u32 v)
{
    char buf[11];
    int  i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i--)
        kputc(buf[i]);
}

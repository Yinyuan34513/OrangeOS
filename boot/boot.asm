; ===========================================================================
; Orange OS / 橙光 OS -- CHENG GUANG OS  ·  NUBASY
; 橙光随行，流畅无界
;
; Assembly stops where C starts.  Stage 1 is a 512-byte MBR that loads two
; things off the disk: stage 2 (this file's second half, real mode) and the C
; payload (src/main.c, linked at 0x10000).  Stage 2 asks the BIOS for a VBE
; linear framebuffer, opens the A20 line, enters protected mode, parks the
; boot information at 0x7000 and calls c_start().
;
; Diagnostics go out of COM1, because the text screen stops being visible the
; moment the graphics mode is set.
; ===========================================================================

BITS 16
org 0x7C00

STAGE2_SECTORS  equ 8                   ; 4 KiB: this file's stage 2
PAYLOAD_SECTORS equ 48                  ; 20 KiB: payload + fonts + mem code
STAGE2_ADDR     equ 0x7E00              ; right after this sector
PAYLOAD_SEG     equ 0x1000              ; payload lands at 0x1000:0x0000
PAYLOAD_ADDR    equ 0x10000
BOOTINFO        equ 0x7000
E820_COUNT      equ 0x6000              ; dword count, entries follow at 0x6010
E820_MAX        equ 30
VBE_MODE        equ 0x0118              ; 1024x768, 24bpp on this BIOS

; ---------------------------------------------------------------------------
; Stage 1
; ---------------------------------------------------------------------------
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    call serial_init
    mov si, msg_hello
    call sprint16

    ; Does this BIOS do extended reads?  They take a plain LBA, which makes
    ; the CHS fallback below the exception rather than the rule.
    mov byte [have_ext], 0
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext
    mov byte [have_ext], 1
.no_ext:

    xor bx, bx
    mov es, bx                          ; stage 2 -> 0x0000:0x7E00
    mov bx, STAGE2_ADDR
    mov bp, 1                           ; 0-based LBA
    mov di, STAGE2_SECTORS
    call load_blocks
    jc .failed

    mov bx, PAYLOAD_SEG                 ; payload -> 0x1000:0x0000
    mov es, bx
    xor bx, bx
    mov bp, 1 + STAGE2_SECTORS
    mov di, PAYLOAD_SECTORS
    call load_blocks
    jc .failed

    jmp dword 0x0000:STAGE2_ADDR

.failed:
    mov si, msg_failed
    call sprint16
.hang:
    hlt
    jmp .hang

; ---- loading --------------------------------------------------------------
; es:bx = destination, bp = 0-based LBA, di = sector count.  CF set on error.
load_blocks:
    cmp byte [have_ext], 0
    je .chs

    mov [dap_off], bx
    mov [dap_seg], es
    mov [dap_cnt], di
    movzx eax, bp
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .ok

    ; The extended read gave up part way; do the whole thing again by CHS,
    ; which rewrites everything it asked for.
.chs:
.next:
    mov ax, bp
    call lba_to_chs                     ; clobbers cx, so the LBA lives in bp
    mov ax, 0x0201                      ; read one sector
    mov dl, [boot_drive]
    int 0x13
    jc .err
    add bx, 512
    inc bp
    dec di
    jnz .next
.ok:
    clc
    ret
.err:
    stc
    ret

; ax = 0-based LBA -> ch/cl/dh.  Geometry is fixed at 18 sectors, 2 heads,
; which is the only geometry a 1.44M image has.  Clobbers cx and dx.
lba_to_chs:
    push bx
    xor dx, dx
    mov bx, 18
    div bx                              ; ax = LBA/18, dx = LBA%18
    mov cx, dx
    inc cx                              ; sector 1..18
    xor dx, dx
    mov bx, 2
    div bx                              ; ax = track, dx = head
    mov dh, dl
    mov ch, al                          ; track bits 0..7
    mov dl, ah                          ; track bits 8..9
    shl dl, 6
    or cl, dl
    pop bx
    ret

; ---- COM1 at 115200 8N1, so diagnostics survive the graphics switch -------
serial_init:
    mov dx, 0x3F9                       ; interrupt enable: none
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80                        ; DLAB on
    out dx, al
    mov dx, 0x3F8                       ; divisor 1 -> 115200 baud
    mov al, 0x01
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03                        ; 8N1, DLAB off
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7                        ; FIFO on, clear both
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B                        ; DTR | RTS | OUT2
    out dx, al
    ret

; dx = port, al = byte
serial_putc:
    push ax
    mov ah, al
    mov dx, 0x3FD                       ; line status
.wait:
    in al, dx
    test al, 0x20                       ; transmitter holding register empty
    jz .wait
    mov dx, 0x3F8
    mov al, ah
    out dx, al
    pop ax
    ret

sprint16:
    lodsb
    or al, al
    jz .done
    call serial_putc
    jmp sprint16
.done:
    ret

; eax -> decimal
sprint_dec:
    pusha
    mov ebx, 10
    xor ecx, ecx
.div:
    xor edx, edx
    div ebx
    push edx
    inc ecx
    test eax, eax
    jnz .div
.emit:
    pop eax
    add al, '0'
    call serial_putc
    loop .emit
    popa
    ret

; ---- stage 1 data ---------------------------------------------------------
boot_drive: db 0
have_ext:   db 0
msg_hello:  db "Orange OS", 0x0D, 0x0A, 0
msg_failed: db "Orange OS: cannot load stage 2", 0x0D, 0x0A, 0

dap:
    db 0x10
    db 0x00
dap_cnt: dw 0
dap_off: dw 0
dap_seg: dw 0
dap_lba: dd 0

    times 510 - ($ - $$) db 0
    dw 0xAA55

; ---------------------------------------------------------------------------
; Stage 2 -- loaded at 0x7E00, still in real mode on entry
; ---------------------------------------------------------------------------
stage2:
    ; The payload load above leaves es at 0x1000.  Every VBE call takes its
    ; buffer in es:di, so this has to go back to zero first.
    xor ax, ax
    mov es, ax

    mov si, msg_vbe
    call sprint16

    ; The info block must announce "VBE2" or the BIOS answers with the 1.x
    ; layout and leaves the framebuffer address out.
    mov dword [vbe_info], 0x32454256    ; "VBE2"

    mov ax, 0x4F00
    mov di, vbe_info
    int 0x10
    cmp ax, 0x004F
    jne vbe_failed

    mov ax, 0x4F01
    mov cx, VBE_MODE
    mov di, mode_info
    int 0x10
    cmp ax, 0x004F
    jne vbe_failed

    mov ax, 0x4F02
    mov bx, VBE_MODE | 0x4000           ; bit 14: use the linear framebuffer
    int 0x10
    cmp ax, 0x004F
    jne vbe_failed

    ; Park what C will need at BOOTINFO, where main.c finds it.
    mov eax, [mode_info + 40]           ; PhysBasePtr
    mov [BOOTINFO + 0], eax
    movzx eax, word [mode_info + 18]    ; XResolution
    mov [BOOTINFO + 4], eax
    movzx eax, word [mode_info + 20]    ; YResolution
    mov [BOOTINFO + 8], eax
    movzx eax, word [mode_info + 16]    ; BytesPerScanLine
    mov [BOOTINFO + 12], eax
    movzx eax, byte [mode_info + 25]    ; BitsPerPixel (24 on this BIOS)
    mov [BOOTINFO + 16], eax

    mov si, msg_fb
    call sprint16
    mov eax, [BOOTINFO + 0]
    call sprint_hex
    mov al, ' '
    call serial_putc
    mov eax, [BOOTINFO + 4]
    call sprint_dec
    mov al, 'x'
    call serial_putc
    mov eax, [BOOTINFO + 8]
    call sprint_dec
    mov al, ' '
    call serial_putc
    mov eax, [BOOTINFO + 16]
    call sprint_dec
    mov al, 'b'
    call serial_putc
    mov al, 0x0D
    call serial_putc
    mov al, 0x0A
    call serial_putc

    ; ---- e820 memory map -------------------------------------------------
    ; Only the BIOS can answer "how much RAM is there and where", and only
    ; real mode can call the BIOS.  This is the last real-mode errand: the
    ; map goes to 0x6000 (count, then 24-byte entries) for the C payload.
    ;
    ; The count lives in memory, not in a register: the e820 contract only
    ; promises EBX and ES:DI, and this BIOS clobbers BP.  DI is recomputed
    ; from the count each round for the same reason.
    mov word [E820_COUNT], 0
    xor ebx, ebx                        ; continuation id: 0 = first call.
                                        ; ebx still holds a buffer offset from
                                        ; the payload load above.
.map:
    mov di, 0x6010
    mov ax, [E820_COUNT]
    mov cx, ax
    shl ax, 4                           ; count * 16
    add di, ax
    mov ax, cx
    shl ax, 3                           ; count * 8
    add di, ax                          ; di = 0x6010 + count * 24
                                        ; (BX holds the continuation id and
                                        ; must reach int 0x15 untouched)

    mov eax, 0x0000E820                 ; subfunction, 0 in the high half
    mov edx, 0x534D4150                 ; "SMAP"
    mov ecx, 24                         ; buffer size we provide
    int 0x15
    jc .done
    cmp eax, 0x534D4150                 ; BIOS must acknowledge the magic
    jne .done
    inc word [E820_COUNT]
    cmp word [E820_COUNT], E820_MAX
    jae .done
    test ebx, ebx                       ; continuation id 0 = last entry
    jz .done
    jmp .map
.done:

    mov si, msg_e820
    call sprint16
    movzx eax, word [E820_COUNT]
    call sprint_dec
    mov al, 0x0D
    call serial_putc
    mov al, 0x0A
    call serial_putc

    ; The framebuffer sits above 1 MiB, so the A20 line has to be open
    ; before it can be addressed at all.
    in al, 0x92
    or al, 0x02
    out 0x92, al

    cli
    lgdt [gdtr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm32

vbe_failed:
    mov si, msg_novbe
    call sprint16
.hang:
    hlt
    jmp .hang

msg_vbe:   db "Orange OS: VBE ok, asking for 0x0118", 0x0D, 0x0A, 0
msg_novbe: db "Orange OS: VBE refused 0x0118", 0x0D, 0x0A, 0
msg_fb:    db "Orange OS: fb=0x", 0
msg_e820:  db "Orange OS: e820 entries=", 0

gdt:
    dq 0x0000000000000000               ; null
    dq 0x00CF9A000000FFFF               ; flat 32-bit code
    dq 0x00CF92000000FFFF               ; flat 32-bit data
gdtr:
    dw 23
    dd gdt

; eax -> hex
sprint_hex:
    pusha
    mov ecx, 8
.rot:
    rol eax, 4
    push eax
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    call serial_putc
    pop eax
    loop .rot
    popa
    ret

vbe_info:  times 512 db 0
mode_info: times 256 db 0

BITS 32
; ---------------------------------------------------------------------------
; 32-bit: hand over to C
; ---------------------------------------------------------------------------
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x0009C000

    call PAYLOAD_ADDR                   ; c_start() in src/main.c

.hang:
    hlt
    jmp .hang

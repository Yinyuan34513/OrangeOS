# Orange OS / 橙光 OS -- CHENG GUANG OS · NUBASY
#
# Three things get built: the fonts (mkfont.py rasterizes the system fonts),
# the assembly (a 512-byte MBR plus a real-mode stage that sets the graphics
# mode and enters protected mode), and the C payload that does the painting.
# All three are padded into a 1.44M floppy image that QEMU, a real machine or
# a USB stick will boot.

BUILD   := build
IMG     := $(BUILD)/orangeos.img
BIN     := $(BUILD)/boot.bin
FONTS   := $(BUILD)/fonts.c
PAYLOAD := $(BUILD)/payload

# Must match STAGE2_SECTORS / PAYLOAD_SECTORS in boot/boot.asm.
STAGE2_SECTORS  := 8
PAYLOAD_SECTORS := 48

NASM      ?= nasm
QEMU      ?= qemu-system-x86_64
QEMUIMG   ?= qemu-img
PYTHON    ?= python3
FFMPEG    ?= ffmpeg
CC        ?= gcc-13
OBJCOPY   ?= objcopy

# No SSE (the A20/CR0 work leaves the FPU off), no red zone games, and no
# PIE -- the payload is linked at a fixed 0x10000.
CFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -nostdlib -nostdinc \
          -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only -O2 -Wall -Wextra -I$(BUILD)

.PHONY: all run console shot test test-irq clean

all: $(IMG)

$(FONTS): tools/mkfont.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/mkfont.py $(FONTS)

$(BIN): boot/boot.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin boot/boot.asm -o $(BIN)
	$(PYTHON) tools/pad.py $(BIN) $$((1 + $(STAGE2_SECTORS)))

$(PAYLOAD).bin: src/main.c src/interrupts.c src/pmm.c src/paging.c src/heap.c src/kputs.c src/task.c $(FONTS) boot/payload.ld
	$(CC) $(CFLAGS) -no-pie -T boot/payload.ld -Wl,--build-id=none \
	    -o $(PAYLOAD).elf src/main.c src/interrupts.c src/pmm.c src/paging.c src/heap.c src/kputs.c src/task.c $(FONTS)
	$(OBJCOPY) -O binary $(PAYLOAD).elf $(PAYLOAD).bin
	$(PYTHON) tools/pad.py $(PAYLOAD).bin $(PAYLOAD_SECTORS)

$(IMG): $(BIN) $(PAYLOAD).bin
	@rm -f $(IMG)
	$(QEMUIMG) create -f raw $(IMG) 1440k >/dev/null
	dd if=$(BIN) of=$(IMG) bs=512 conv=notrunc status=none
	dd if=$(PAYLOAD).bin of=$(IMG) bs=512 seek=$$((1 + $(STAGE2_SECTORS))) \
	    conv=notrunc status=none
	@echo "Orange OS: $(IMG)"

# Boot it in a window.
run: $(IMG)
	$(QEMU) -fda $(IMG) -m 16 -boot a

# The serial shell, attached to this terminal: type at it.
console: $(IMG)
	$(QEMU) -fda $(IMG) -m 16 -boot a -display none -serial stdio

# Automated round-trip: feed commands over COM1 and check the answers came
# back.  '> MARKER' can only be produced by the echo command, never by the
# local echo of what was typed, so it proves the guest really parsed input.
test: $(IMG)
	@rm -f $(BUILD)/console.log
	( sleep 3; printf 'help\r'; sleep 1; printf 'echo MARKER-xyz\r'; sleep 1; \
	  printf 'badcmd\r'; sleep 1; printf 'sysinfo\r'; sleep 1; printf 'kmtest\r'; \
	  sleep 1; printf 'poweroff\r'; sleep 2 ) | \
	timeout 25 $(QEMU) -fda $(IMG) -m 16 -boot a -display none -no-reboot \
	    -serial stdio > $(BUILD)/console.log 2>&1
	@grep -q "^> MARKER-xyz" $(BUILD)/console.log && \
	grep -q "kmtest: PASS" $(BUILD)/console.log && \
	grep -q "unknown command: badcmd" $(BUILD)/console.log && \
	grep -q "CPU vendor:" $(BUILD)/console.log && echo "Orange OS: serial I/O PASS"

# The keyboard interrupt path, end to end: the string is typed by injecting
# PS/2 make codes at the monitor, so it travels through IRQ1, the scancode
# tables and the ring buffer before the shell ever sees it.  '> KBDIRQ' can
# only exist if every one of those stages worked, shift included.
test-irq: $(IMG)
	@rm -f $(BUILD)/irq.log $(BUILD)/irqmon.sock
	( $(QEMU) -fda $(IMG) -m 16 -boot a -display none -no-reboot \
	    -serial file:$(BUILD)/irq.log \
	    -monitor unix:$(CURDIR)/$(BUILD)/irqmon.sock,server,nowait \
	    >/dev/null 2>&1 & )
	@sleep 5
	@$(PYTHON) tools/sendkeys.py $(BUILD)/irqmon.sock "echo KBDIRQ\nsysinfo\n"
	@sleep 2
	@grep -q "^> KBDIRQ" $(BUILD)/irq.log && \
	grep -qE "interrupts: ticks [1-9]" $(BUILD)/irq.log && \
	grep -qE "keys [1-9]" $(BUILD)/irq.log && \
	echo "Orange OS: keyboard IRQ PASS"

# Boot headless and dump the framebuffer to build/screen.png.  There is no
# console after the VBE switch, so a screenshot is the only evidence the
# painting actually happened; serial.log gets the diagnostics.
shot: $(IMG)
	@rm -f $(BUILD)/screen.ppm $(BUILD)/mon.sock
	( $(QEMU) -fda $(IMG) -m 16 -boot a -display none \
	    -serial file:$(BUILD)/serial.log \
	    -monitor unix:$(CURDIR)/$(BUILD)/mon.sock,server,nowait >/dev/null 2>&1 & )
	@sleep 5
	@$(PYTHON) tools/screenshot.py $(BUILD)/mon.sock $(BUILD)/screen.ppm
	@$(FFMPEG) -nostdin -y -loglevel error -i $(BUILD)/screen.ppm $(BUILD)/screen.png
	@echo "Orange OS: $(BUILD)/screen.png"

clean:
	rm -rf $(BUILD)

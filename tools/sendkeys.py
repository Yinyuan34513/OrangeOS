#!/usr/bin/env python3
"""
Type a string into a running QEMU by driving the PS/2 keyboard through the
monitor's sendkey command.

This is how the keyboard interrupt path gets tested: the characters go
through the real IRQ1 handler -- scancode decode, shift, ring buffer -- and
what comes out on the serial console proves the whole chain worked.  socat
cannot be used here because the QEMU monitor never closes the connection;
see tools/screenshot.py for the same reasoning in more detail.
"""

import socket
import sys
import time

sock_path = sys.argv[1]
text = sys.argv[2] if len(sys.argv) > 2 else ""

NAMES = {
    " ": "spc",
    "\n": "ret",
    "\r": "ret",
    "-": "minus",
    ".": "dot",
    ",": "comma",
    ";": "semicolon",
    "=": "equal",
    "/": "slash",
}


def command(s, text, settle=0.25):
    s.sendall(text.encode() + b"\n")
    time.sleep(settle)
    try:
        s.settimeout(1.0)
        s.recv(65536)
    except socket.timeout:
        pass


for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        break
    except (FileNotFoundError, ConnectionRefusedError):
        time.sleep(0.1)
else:
    sys.exit(f"sendkeys: {sock_path} never appeared")

s.settimeout(2.0)
command(s, "")  # drain the banner

# The caller writes "\n" inside a make recipe, where nothing interprets it.
text = text.replace("\\n", "\n").replace("\\r", "\r")

for ch in text:
    if ch.isupper():
        name = "shift-" + ch.lower()
    else:
        name = NAMES.get(ch, ch)
    command(s, "sendkey " + name, settle=0.12)

command(s, "quit")
s.close()
print("sendkeys: done")

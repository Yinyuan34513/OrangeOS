#!/usr/bin/env python3
"""
Ask a running QEMU (started with -monitor unix:...) for a screendump.

socat turns out to be a bad fit for this: the QEMU monitor never closes the
connection, so socat waits for an EOF that never comes and make hangs.  This
does the same job with a timeout on every read and exits on its own.
"""

import os
import socket
import sys
import time

sock_path = sys.argv[1]
out_path = sys.argv[2]

# The monitor socket appears a moment after QEMU starts.
for _ in range(50):
    if os.path.exists(sock_path):
        break
    time.sleep(0.1)
else:
    sys.exit(f"screenshot: {sock_path} never appeared")

s = socket.socket(socket.AF_UNIX)
s.connect(sock_path)
s.settimeout(2.0)


def command(text, settle=0.5):
    s.sendall(text.encode() + b"\n")
    time.sleep(settle)
    try:
        s.recv(65536)  # drain the banner / prompt / completion noise
    except socket.timeout:
        pass


command(f"screendump {os.path.abspath(out_path)}", settle=1.0)
command("quit")

s.close()

if not os.path.exists(out_path):
    sys.exit("screenshot: QEMU never wrote the dump")
print(f"screenshot: {out_path} ({os.path.getsize(out_path)} bytes)")

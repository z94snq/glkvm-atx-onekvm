#!/usr/bin/env python3
"""
GL-ATXPC board simulator, for testing the bridge without the board.

Creates a pseudo-terminal that answers like the board (protocol from
dsetareh/glkvm-atx-pikvm) and logs every command with a timestamp:

    GET_POWER_STATE       -> "RCV: GET_POWER_STATE", then 0 (off) / 1 (on)
    POWER_SW              -> "RCV: POWER_SW"        toggles the simulated PC on/off
    POWER_SW_FORCE <ms>   -> "RCV: POWER_SW_FORCE"  turns it off if ms >= 4000
    POWER_RESET           -> "RCV: POWER_RESET"
    GET_SN                -> "RCV: GET_SN", then a fake serial number

Usage:
    python3 glatx_sim.py /dev/ttyGLSIM       (creates the link; needs write access there)

Then point the bridge at it: GLATX_DEVICE=/dev/ttyGLSIM
"""

import os
import pty
import sys
import termios
import time
import tty

link = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ttyGLSIM"
master, slave = pty.openpty()
tty.setraw(slave)
attrs = termios.tcgetattr(slave)
attrs[3] &= ~termios.ECHO
termios.tcsetattr(slave, termios.TCSANOW, attrs)
name = os.ttyname(slave)
tmp = link + ".tmp"
if os.path.lexists(tmp):
    os.remove(tmp)
os.symlink(name, tmp)
os.replace(tmp, link)
start = time.monotonic()
powered = False


def log(msg: str) -> None:
    print(f"[sim {time.monotonic() - start:8.3f}s] {msg}", flush=True)


def reply(*lines: str) -> None:
    os.write(master, "".join(line + "\r\n" for line in lines).encode())


log(f"GL-ATXPC simulator on {link} -> {name}, PC is off")
buf = b""
while True:
    buf += os.read(master, 256)
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        cmd = raw.decode("ascii", "replace").strip()
        if not cmd:
            continue
        token = cmd.split()[0]
        if token == "GET_POWER_STATE":
            reply("RCV: GET_POWER_STATE", "1" if powered else "0")
        elif token == "GET_SN":
            reply("RCV: GET_SN", "SIM0000001")
        elif token == "POWER_SW":
            reply("RCV: POWER_SW")
            powered = not powered
        elif token == "POWER_SW_FORCE":
            reply("RCV: POWER_SW_FORCE")
            ms = int(cmd.split()[1]) if len(cmd.split()) > 1 else 0
            if ms >= 4000:
                powered = False
        elif token == "POWER_RESET":
            reply("RCV: POWER_RESET")
        else:
            log(f"unknown command {cmd!r} (no reply)")
            continue
        log(f"{cmd:<22} PC is now {'on' if powered else 'off'}")

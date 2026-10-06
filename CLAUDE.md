# Project: glkvm-atx-onekvm

Bridge that lets One-KVM drive the GL.iNet GL-ATXPC ATX board (USB 1209:c550) with no
One-KVM code changes. One-KVM counterpart of dsetareh/glkvm-atx-pikvm (a kvmd plugin).
One-KVM has no plugin system (ATX drivers are a fixed compiled-in list), so the bridge
pretends to be an LCUS serial relay, which One-KVM supports.

## Hardware setup
- x86 PC "hinode" (Ubuntu 26.04) running One-KVM v0.2.8 in Docker
  (~/one-kvm/docker-compose.yml, official image silentwind0/one-kvm-full, privileged,
  /dev and /sys mounted, data in ~/one-kvm/config)
- HID: ESP32-S3 CH9329 emulator on /dev/ttyACM0 (repo s3-onekvm-hid); not related, but
  it is also a ttyACM device, so the bridge must find the board by USB ID, never by number
- GL-ATXPC: NOT owned yet (planned purchase). Everything so far is tested against
  tools/glatx_sim.py.

## Decisions (made with the user 2026-10-06, don't revisit without asking)
- No One-KVM code changes: no fork, no custom image. One-KVM has no plugin system.
- No kernel modules. A gpio-sim bridge (which would also give the power LED) was tested
  and rejected for this reason; the user does not need power LED status.
- Bridge = fake LCUS serial relay over a pty, as a static C binary running INSIDE the
  One-KVM container (image has no Python), started by the compose `command:` before /init.sh.
  This keeps the pty alive exactly as long as One-KVM, so a bridge restart can never leave
  One-KVM holding a dead port.

## Architecture
- `core.c`: LCUS frame decoding + press mapping, no I/O (host-testable)
- `main.c`: pty + symlink /dev/ttyACM-glatx, board serial I/O (open per command, like the
  reference driver), poll() loop with core_next_timeout()
- `tools/glatx_sim.py`: board simulator on a pty
- `test/test_core.c`: host tests
- `build.sh`: static build in gcc:12-bookworm

## One-KVM facts (verified from its source, src/atx/; don't change without re-reading)
- LCUS serial relay frame: [0xA0][channel 1-8][state 0/1][sum of the 3 bytes]; write-only,
  One-KVM never reads a reply. Init writes "off" to each enabled channel.
- Power and reset share one serial port when both are enabled.
- Press timings (executor.rs): short 500 ms, long 5000 ms, reset 500 ms (on, sleep, off)
- ATX devices are opened only at startup or when ATX settings are saved; no retry.
- The ATX device field is a dropdown: serialport::available_ports() plus /dev entries
  starting with ttyUSB or ttyACM. A pty is only listed through a ttyACM*/ttyUSB* name.
- Power/HDD LED inputs are GPIO only.
- The One-KVM container shares the host's /dev/pts (privileged + /dev bind mount), so a pty
  made by another privileged container with /dev mounted is reachable (used by the simulator).

## Board protocol (from glkvm-atx-pikvm, not yet verified on real hardware)
- ASCII lines over CDC-ACM, "\n" terminated; board echoes "RCV: <TOKEN>" (TOKEN = first word)
- POWER_SW (~0.5 s press), POWER_SW_FORCE <ms>, POWER_RESET, GET_SN (+ payload line),
  GET_POWER_STATE (+ payload 0/1/2), UPDATE (bootloader: never send)

## Status
- 2026-10-06: host tests pass. End-to-end with the simulator: emulated One-KVM frames →
  bridge → simulator give POWER_SW at release (+0.5 s), POWER_SW_FORCE 6500 at +1.5 s,
  POWER_RESET immediately.
- 2026-10-06: deployed in ~/one-kvm (binary in ~/one-kvm/glatx/). Real One-KVM UI clicks
  (power short, long, reset) reached the simulator correctly. After `docker compose restart`
  the bridge made the port before One-KVM's ATX init and One-KVM held the new pty
  (same /dev/pts as the link), no re-save needed.
- 2026-10-06 double check: failure paths in isolation (board silent -> "no echo" after 1 s,
  board missing, same channel for power/reset -> exit 1, invalid env -> default). On One-KVM:
  binary missing -> One-KVM up after 5.5 s without ATX ("Serial port open failed"), restoring
  the binary + restart brings ATX back.
- The link /dev/ttyACM-glatx stays (dangling) while the container is stopped: when One-KVM
  (PID 1) exits, the kernel SIGKILLs the rest of the PID namespace at once, so the bridge
  can't clean up (PR_SET_PDEATHSIG was tried and doesn't help). The wrapper's rm -f and the
  bridge's atomic rename replace it on the next start.
- Deploy updates with `install -m 755` (cp fails with "Text file busy" while it runs).
- Not tested: real GL-ATXPC (line endings, echo format, POWER_SW_FORCE timing), re-plugging
  the real board.

## Rules
- Never send UPDATE to the board. Startup sends only GET_SN (read-only).
- Find the board by USB ID 1209:c550, never by ttyACM number.
- After any change to core.c, run the host tests:
```sh
  gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/test_core.c core.c
  ./hosttest
```
- Build with ./build.sh (static, -Werror) and fix all warnings.
- License GPL-3.0-or-later (protocol from glkvm-atx-pikvm, GPL-3.0).
- Don't add features (power LED, kernel modules, One-KVM patches) unless I ask.

## Next tasks, in order
1. When the GL-ATXPC arrives: plug it in, check `docker logs one-kvm | grep glatx` for its
   serial number (production compose already auto-detects; the simulator setup was removed).
2. With the board wired to a test PC: short press, long press, reset from the One-KVM UI.
3. Unplug/replug the board while One-KVM runs; press again.

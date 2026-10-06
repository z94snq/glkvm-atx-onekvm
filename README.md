# glkvm-atx-onekvm

Lets [One-KVM](https://github.com/mofeng-git/One-KVM) press a PC's power and reset
buttons through the [GL.iNet GL-ATXPC "ATX Board"](https://docs.gl-inet.com/kvm/en/user_guide/gl-atx-board/)
(silk-screened `KVM-ATX-V1.0`, USB `1209:c550`) by **emulating an LCUS serial relay**,
with no changes to One-KVM.

It is the One-KVM counterpart of [glkvm-atx-pikvm](https://github.com/dsetareh/glkvm-atx-pikvm),
which is a kvmd plugin. One-KVM has no plugin system and no driver for this board,
so this is a small bridge instead:

```
One-KVM ATX (driver "Serial", LCUS relay) ──► /dev/ttyACM-glatx (fake serial port)
                                                   │ glatx-bridge
GL-ATXPC (USB 1209:c550, ASCII protocol) ◄─────────┘ POWER_SW / POWER_SW_FORCE / POWER_RESET
```

`glatx-bridge` creates a fake serial port that looks like an LCUS serial relay,
which One-KVM supports, and turns One-KVM's relay pulses into GL-ATXPC commands.
It is a static binary that runs inside the One-KVM container, started just
before One-KVM, so the port always exists when One-KVM opens it and both restart together.

What works: power button (short press), power button long press (force off),
reset button, from One-KVM's web UI and API. The board is found by USB ID on every
press, so re-plugging it needs no restart. No kernel modules, no extra container,
official One-KVM image.

Not supported: power LED status in One-KVM (One-KVM reads LEDs only from GPIO),
HDD LED (not on the board).

## How presses map

The board can't hold a button for an arbitrary time, so the bridge maps One-KVM's
fixed presses (500 ms short, 5000 ms long, 500 ms reset) like this:

| One-KVM | Relay | Board command | Reaches the PC |
| --- | --- | --- | --- |
| Power (short) | channel 1, 500 ms | `POWER_SW` (board presses ~0.5 s) | at the release, ~0.5 s after the click |
| Power (long) | channel 1, 5000 ms | `POWER_SW_FORCE 6500` | 1.5 s into the press |
| Reset | channel 2, 500 ms | `POWER_RESET` | immediately |

One-KVM only writes to the relay and never reads, so it reports a click as done
even if the board is unplugged. The bridge logs every command it sends, and every failure.

## Board protocol

Line-based ASCII over USB CDC-ACM (baud rate is irrelevant), reverse-engineered
by glkvm-atx-pikvm from GL.iNet's `/sbin/atxpower`. Every command is echoed as
`RCV: <COMMAND>`:

| Host sends | Board replies | Meaning |
| --- | --- | --- |
| `POWER_SW` | `RCV: POWER_SW` | short power press (~0.5 s) |
| `POWER_SW_FORCE <ms>` | `RCV: POWER_SW_FORCE` | long press for `<ms>` |
| `POWER_RESET` | `RCV: POWER_RESET` | reset press |
| `GET_SN` | `RCV: GET_SN`, then the serial number | used once at startup, presses nothing |
| `GET_POWER_STATE` | `RCV: GET_POWER_STATE`, then `0`/`1`/`2` | not used (One-KVM can't show it) |
| `UPDATE` | | bootloader mode; never sent |

## Wiring

Per [GL.iNet's guide](https://docs.gl-inet.com/kvm/en/user_guide/gl-atx-board/):
connect the board's harness to the target motherboard's front-panel header
(power switch and reset switch pairs), and plug the board's USB-C into the
One-KVM host. It shows up as `/dev/ttyACMn`; the bridge finds it by USB ID.

## Install

1. Build the bridge (needs only Docker):

   ```sh
   ./build.sh
   ```

2. Copy it next to your One-KVM compose file:

   ```sh
   mkdir -p ~/one-kvm/glatx
   install -m 755 glatx-bridge ~/one-kvm/glatx/
   ```

   Use `install`, not `cp`: when updating, `cp` fails with "Text file busy" while
   the old bridge is running. Restart One-KVM afterwards to run the new one.

3. Add the volume and the `command` below to the One-KVM service in
   `docker-compose.yml` (the official image's own command is `/init.sh`):

   ```yaml
   services:
     one-kvm:
       image: silentwind0/one-kvm-full:latest
       privileged: true
       network_mode: host
       volumes:
         - ~/one-kvm/config:/etc/one-kvm
         - ~/one-kvm/glatx:/glatx:ro
         - /dev:/dev
         - /sys:/sys
       # Start the GL-ATXPC bridge before One-KVM, so its fake relay port exists
       # when One-KVM opens ATX
       command:
         - /bin/sh
         - -c
         - |
           rm -f /dev/ttyACM-glatx
           /glatx/glatx-bridge &
           i=0; while [ ! -e /dev/ttyACM-glatx ] && [ $$i -lt 50 ]; do sleep 0.1; i=$$((i+1)); done
           exec /init.sh
       restart: unless-stopped
   ```

   If the bridge is missing, One-KVM still starts after 5 seconds, without ATX.

4. `docker compose up -d`, then check the bridge's lines in One-KVM's log:

   ```sh
   docker logs one-kvm 2>&1 | grep glatx
   # [glatx] Fake LCUS relay port: /dev/ttyACM-glatx -> /dev/pts/3
   # [glatx] Board /dev/ttyACM1, serial number ...
   ```

5. In the One-KVM web UI, Settings → ATX:

   - ATX: on
   - Driver: **Serial**
   - Device: **/dev/ttyACM-glatx** (click refresh if it isn't listed)
   - Baud rate: any (9600 is fine; it's a fake port)
   - Power button: enabled, channel **1**
   - Reset button: enabled, channel **2**

   Don't pick the board's own `/dev/ttyACMn`: the bridge talks to it.

`docker compose pull` / `up -d` updates of One-KVM keep working: the bridge is
mounted from outside the image.

## Configuration

Environment variables on the One-KVM service, all optional:

| Variable | Default | Meaning |
| --- | --- | --- |
| `GLATX_DEVICE` | auto-detect USB `1209:c550` | Board serial device |
| `GLATX_LINK` | `/dev/ttyACM-glatx` | Fake port path. One-KVM only lists `/dev/ttyACM*` and `/dev/ttyUSB*` names |
| `GLATX_POWER_CHANNEL` | `1` | One-KVM power button channel |
| `GLATX_RESET_CHANNEL` | `2` | One-KVM reset button channel |
| `GLATX_LONG_AFTER_MS` | `1500` | A power press held this long is a long press |
| `GLATX_LONG_PRESS_MS` | `6500` | `POWER_SW_FORCE` duration (same as GL.iNet's tool) |
| `GLATX_TIMEOUT_MS` | `1000` | Wait for the board's `RCV:` echo |

## Test without the board

`tools/glatx_sim.py` simulates the board on a fake serial port and logs every
command, with a simulated PC that turns on and off:

```sh
docker run -d --name glatx-sim --privileged -v /dev:/dev -v "$PWD/tools":/t:ro \
    python:3.13-slim python3 -u /t/glatx_sim.py /dev/ttyGLSIM
```

Add `GLATX_DEVICE=/dev/ttyGLSIM` to the One-KVM service, `docker compose up -d`,
click the buttons in One-KVM, and watch `docker logs -f glatx-sim`. This works
because One-KVM runs privileged with `/dev` from the host, so both containers
share the host's `/dev/pts`.

## Troubleshooting

**`/dev/ttyACM-glatx` is not in One-KVM's device list.** Check
`docker logs one-kvm | grep glatx` for the "Fake LCUS relay port" line, then click
refresh next to the driver selector.

**Clicks succeed in One-KVM but nothing happens.** One-KVM can't tell; look at the
bridge's log. "board not found" means the USB cable or the board; "no echo"
means the board didn't answer.

**`/dev/ttyACM-glatx` still exists while One-KVM is stopped.** Expected: when the
container stops, the kernel kills the bridge before it can remove its link. The link
then points at nothing, and the next start replaces it.

**ATX stopped working after editing the compose file.** One-KVM opens the fake
port only when it starts or when ATX settings are saved. Restart the container
(`docker compose up -d` does it after a compose change), or re-save the ATX settings.

## Developing

`core.c` holds the relay decoding and press mapping with no I/O, so it can be
tested on a PC:

```sh
gcc -std=c11 -Wall -Wextra -O2 -o hosttest test/test_core.c core.c
./hosttest
```

The tests replay One-KVM's frames and timings: startup "off" frames, short and
long power presses, reset, bad checksums, split and back-to-back frames, other channels.

The One-KVM behavior comes from its `src/atx/serial_relay.rs` (frame format,
write-only), `src/atx/executor.rs` (press timings) and `src/atx/mod.rs`
(device discovery).

| File | Purpose |
| --- | --- |
| `core.c` | LCUS frame decoding and press → board command mapping |
| `main.c` | Fake port (pty), board serial I/O, main loop |
| `build.sh` | Static build in Docker |
| `tools/glatx_sim.py` | Board simulator |
| `test/` | Host tests of the core |

## License

GPL-3.0-or-later. The board protocol is from
[glkvm-atx-pikvm](https://github.com/dsetareh/glkvm-atx-pikvm) (GPL-3.0).

# AtomKey

A WinKey-protocol-compatible CW keyer running on an **M5Stack Atom Lite**
(ESP32-PICO-D4). It presents a USB serial port that speaks enough of the K1EL
WinKey host protocol that a logging program can drive it like a real WinKeyer
— but element timing is generated on the ESP32, not on the host PC.

This is a cheap bench device, primarily for developing and testing the
`WinKeyerClient` backend in [ContestLogX](https://github.com/sjwoodr/ContestLogX). The two projects
meet only at the WinKey serial protocol: AtomKey has no dependency on CLX, and
CLX cannot tell AtomKey apart from a genuine K1EL WinKeyer.

> "WinKey" / "WinKeyer" are product names of K1EL. AtomKey is an independent,
> protocol-compatible implementation and is not affiliated with K1EL.

## Why this exists

Keying contest CW through flrig (XML-RPC over TCP, contending with a polling
loop) adds latency and jitter. Offloading element generation to dedicated
hardware — the approach a real WinKeyer takes — fixes both. AtomKey lets us
build and test the host side without buying a WinKeyer first.

## Hardware

| Function        | Pin      | Notes                                              |
|-----------------|----------|----------------------------------------------------|
| RGB status LED  | GPIO 27  | Built in. Amber = key down, dim blue = idle/open.  |
| Button          | GPIO 39  | Built in. Press = send a self-test message.        |
| Sidetone        | GPIO 22  | Side header. Attach a **passive piezo buzzer** to GND. |
| Key out         | GPIO 23  | Side header. Active-high; opto/transistor to rig KEY. Safe to leave unconnected. |
| PTT out         | GPIO 25  | Side header. Active-high; opto/transistor to rig PTT. Safe to leave unconnected. |

All user-wired signals are on the **2.54 mm side headers**, so the whole build
uses ordinary Dupont jumpers (the Grove port is left free).

**No rig is needed.** For audible CW, put a passive piezo buzzer between
GPIO 22 and GND. With nothing attached you still get visual keying on the LED.

> Do not key a rig directly from a 3.3 V GPIO — when you get there, put an
> optocoupler or transistor between `KEY_OUT_PIN` and the rig's KEY line.

See **[docs/wiring.md](docs/wiring.md)** for the buzzer hookup, the PC817
optocoupler (and 2N3904) keying stage, the PTT stage, and a shopping list.

## Toolchain (PlatformIO)

```bash
# Install once (Python required):
pip install --user platformio        # or: pipx install platformio

# Build:
pio run

# Flash (Atom Lite plugged in via a DATA-capable USB-C cable):
pio run -t upload

# Watch the serial port (raw WinKey bytes, not text):
pio device monitor
```

> **Set your serial port before flashing.** `platformio.ini` pins
> `upload_port` / `monitor_port` to `/dev/ttyUSB1`, which is just the author's
> machine — **you must change these to match your own port.** Run
> `pio device list` to find it. Note the Atom Lite enumerates as an FTDI
> FT232R (VID:PID `0403:6001`, description `M5stack`), the same chip family as
> some rig CAT cables, so auto-detect can pick the wrong port — pinning it
> explicitly avoids flashing the wrong device. For a binding that survives
> reboots/replugs, point them at the stable symlink under
> `/dev/serial/by-id/` instead of `/dev/ttyUSBn`.

On **Linux** the USB-UART chip is usually in-kernel and shows up as
`/dev/ttyACM0` or `/dev/ttyUSB0`. If you get a permission error, add yourself to
the `dialout` group and re-login:

```bash
sudo usermod -aG dialout $USER
```

> The USB serial port runs at **1200 baud, 8N1** — the WinKey spec. It carries
> the binary protocol, not log text, so the monitor will show raw bytes.

## Protocol status

Implemented:
- Host open (`00 02`) → returns firmware revision (`0x17`, WK2 v2.3)
- Host close (`00 03`), reset (`00 01`), echo test (`00 04`)
- Set WPM (`02`), clear/abort (`0A`), status request (`15`)
- Printable ASCII (`>= 0x20`) queued and sent as Morse
- **Weighting** (`03`), **dit/dah ratio** (`17`), **Farnsworth** (`0D`),
  **key compensation** (`11`) — full element-timing model
- **Sidetone pitch + paddle-only mute** (`01`)
- **PTT lead-in / tail** (`04`) sequenced on GPIO 25
- **Pause** (`06`), **key-immediate / tune** (`0B`), **backspace** (`08`)
- **Mode register** (`0E`): contest word-spacing + serial echo
- **Prosign merge** (`1B`) — concatenated chars, no inter-char gap
- **Load defaults** (`0F`, 15-byte block)
- Status byte with **busy + XOFF backpressure**, emitted on every edge
- Parameter-length table for `0x01..0x1F` (verified vs. WK2 datasheet v23) so
  unimplemented commands stay in sync

Not yet (consumed and ignored): speed pot (`05`/`07`, no pot on Atom Lite),
pin config (`09`), first extension (`10`, stored), paddle switchpoint (`12`),
the buffered family (`18`–`1F` except merge), paddle input. Mode bits for
paddle keying (iambic/ultimatic/swap/watchdog) are stored for M4.

## Milestones

- **M0** — toolchain proof: LED lights on boot. ✅
- **M1** — handshake: 1200-baud host-open returns the revision byte. ✅
- **M2** — keying: set-speed + send-text generate non-blocking element timing
  on the LED + buzzer; abort stops within one loop pass. ✅
- **M3** — robustness: fuller status semantics, weighting, PTT timing.
- **M4** — paddle input + sidetone config.

See [TODO.md](TODO.md) for the tasked-out M3/M4 backlog.

## Notes

WiFi/Bluetooth are never started, so nothing competes with the keying loop.
Timing currently uses `millis()`, which is fine for a bench device; for
production-grade jitter at high WPM, move element timing to a hardware timer
pinned to core 1.

## License

[MIT](LICENSE)

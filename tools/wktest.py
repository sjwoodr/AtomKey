#!/usr/bin/env python3
"""Poke an AtomKey (or any WinKey-compatible keyer) over serial.

Does the WinKey host-open handshake, verifies the revision byte, then drives a
sequence of commands and a message — the same conversation the ContestLogX
WinKeyerClient will have. Each WinKey command has a host-side poke here so new
firmware features can be exercised from the bench.

Usage:
    python3 tools/wktest.py [PORT] [WPM] [MESSAGE]
    python3 tools/wktest.py /dev/ttyUSB1 25 "CQ TEST DE ATOMKEY"

    # Exercise the M3 feature set (weighting, ratio, sidetone, prosign, PTT,
    # serial echo, pause/backspace) instead of a plain send:
    python3 tools/wktest.py /dev/ttyUSB1 25 --demo

Requires pyserial:  python3 -m pip install pyserial
"""
import sys
import time
import serial   # pip install pyserial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
WPM  = int(sys.argv[2]) if len(sys.argv) > 2 else 25
ARG3 = sys.argv[3] if len(sys.argv) > 3 else "CQ TEST DE ATOMKEY "
DEMO = ARG3 == "--demo"
MSG  = (ARG3 if not DEMO else "").encode()

# WinKey runs at 1200 baud, 8N1.
ser = serial.Serial(PORT, 1200, timeout=1)

# Opening the port toggles DTR/RTS, which reboots the ESP32. Wait for boot,
# then clear any boot chatter before we start the protocol.
time.sleep(2)
ser.reset_input_buffer()


def cmd(*bytes_):
    ser.write(bytes(bytes_))


def drain_status(seconds, label="status bytes  -> "):
    """Print whatever the keyer emits (status 0xC0-0xFF, echo chars >= 0x20)."""
    print(label, end="", flush=True)
    end = time.time() + seconds
    while time.time() < end:
        b = ser.read(1)
        if b:
            v = b[0]
            if v >= 0x20 and v < 0x7F:
                print(repr(chr(v)), end=" ", flush=True)   # serial-echo char
            else:
                print(f"{v:02X}", end=" ", flush=True)     # status byte
    print()


# --- Host open: admin(0x00) + open(0x02) -> keyer returns its revision byte
cmd(0x00, 0x02)
rev = ser.read(1)
if rev:
    print(f"host-open OK  -> revision 0x{rev[0]:02X} ({rev[0]})")
else:
    print("host-open FAILED -> no response (wrong port? not running?)")
    ser.close()
    sys.exit(1)

# --- Set speed: 0x02 <wpm>
cmd(0x02, WPM)
print(f"set speed     -> {WPM} WPM")

if not DEMO:
    # --- Send text: printable ASCII (>= 0x20) is keyed as Morse
    ser.write(MSG)
    print(f"sending       -> {MSG.decode()!r}  (watch the LED / listen to the buzzer)")
    drain_status(12)
    ser.close()
    print("done.")
    sys.exit(0)

# ---------------------------------------------------------------------------
# --demo: exercise the M3 command set. Watch the LED/buzzer and the echo line.
# ---------------------------------------------------------------------------
print("\n-- M3 feature demo --")

cmd(0x01, 0x05)              # sidetone pitch -> N=5 (800 Hz), paddle-only bit clear
print("sidetone      -> 800 Hz (0x01 0x05)")

cmd(0x03, 60)               # weighting 60 (heavier than 50/normal)
print("weighting     -> 60 (0x03)")

cmd(0x17, 50)              # dit/dah ratio 50 = 1:3 (normal)
print("dit/dah ratio -> 50 = 1:3 (0x17)")

cmd(0x04, 5, 10)         # PTT lead-in 50 ms, tail 100 ms -> drives GPIO 25
print("PTT lead/tail -> 50 ms / 100 ms (0x04); scope GPIO 25")

cmd(0x0E, 0x04)            # mode register: bit2 = serial echo on
print("mode          -> serial echo on (0x0E 0x04); keyed chars echo below")

ser.write(b"PARIS ")
print("sending       -> 'PARIS ' (echo chars appear as 'P' 'A' ...)")
drain_status(8, "echo + status -> ")

# Prosign: merge A+R into AR with no inter-character gap (0x1B <c1> <c2>)
cmd(0x1B, ord('A'), ord('R'))
print("prosign       -> AR merged (0x1B 'A' 'R')")
drain_status(4)

# Pause / resume mid-buffer
ser.write(b"TEST")
cmd(0x06, 1)               # pause
print("pause         -> on after queueing 'TEST' (0x06 0x01)")
time.sleep(1)
cmd(0x06, 0)               # resume
print("resume        -> (0x06 0x00)")
drain_status(6)

# Tune: continuous key-down then up (0x0B 0x01 / 0x0B 0x00)
cmd(0x0B, 1)
print("tune          -> key down (0x0B 0x01)")
time.sleep(1)
cmd(0x0B, 0)
print("tune          -> key up (0x0B 0x00)")

# Clear/abort to leave the keyer idle
cmd(0x0A)
drain_status(2)
ser.close()
print("done.")

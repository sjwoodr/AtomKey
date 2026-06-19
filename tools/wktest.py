#!/usr/bin/env python3
"""Poke an AtomKey (or any WinKey-compatible keyer) over serial.

Does the WinKey host-open handshake, verifies the revision byte, sets WPM,
sends a message to be keyed as Morse, then prints any status bytes the keyer
emits. This is the same conversation the ContestLogX WinKeyerClient will have.

Usage:
    python3 tools/wktest.py [PORT] [WPM] [MESSAGE]
    python3 tools/wktest.py /dev/ttyUSB1 25 "CQ TEST DE ATOMKEY"

Requires pyserial:  python3 -m pip install pyserial
"""
import sys
import time
import serial   # pip install pyserial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
WPM  = int(sys.argv[2]) if len(sys.argv) > 2 else 25
MSG  = (sys.argv[3] if len(sys.argv) > 3 else "CQ TEST DE ATOMKEY ").encode()

# WinKey runs at 1200 baud, 8N1.
ser = serial.Serial(PORT, 1200, timeout=1)

# Opening the port toggles DTR/RTS, which reboots the ESP32. Wait for boot,
# then clear any boot chatter before we start the protocol.
time.sleep(2)
ser.reset_input_buffer()

# --- Host open: admin(0x00) + open(0x02) -> keyer returns its revision byte
ser.write(bytes([0x00, 0x02]))
rev = ser.read(1)
if rev:
    print(f"host-open OK  -> revision 0x{rev[0]:02X} ({rev[0]})")
else:
    print("host-open FAILED -> no response (wrong port? not running?)")
    ser.close()
    sys.exit(1)

# --- Set speed: 0x02 <wpm>
ser.write(bytes([0x02, WPM]))
print(f"set speed     -> {WPM} WPM")

# --- Send text: printable ASCII (>= 0x20) is keyed as Morse
ser.write(MSG)
print(f"sending       -> {MSG.decode()!r}  (watch the LED / listen to the buzzer)")

# --- Drain status bytes (0xC4 busy / 0xC0 idle) for a few seconds
print("status bytes  -> ", end="", flush=True)
end = time.time() + 12
while time.time() < end:
    b = ser.read(1)
    if b:
        print(f"{b[0]:02X} ", end="", flush=True)
print()

ser.close()
print("done.")

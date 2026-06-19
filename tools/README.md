# AtomKey tools

Host-side utilities for poking an AtomKey (or any WinKey-compatible keyer)
over its serial port. These talk the same WinKey host protocol that the
ContestLogX `WinKeyerClient` backend will use — handy for verifying firmware
without a full logging program attached.

## Setup

Requires [pyserial](https://pyserial.readthedocs.io/):

```bash
python3 -m pip install -r tools/requirements.txt
```

## wktest.py

Runs the full WinKey conversation: host-open handshake, revision check, set
speed, send a message to be keyed as Morse, then print the busy/idle status
bytes the keyer emits.

```bash
python3 tools/wktest.py [PORT] [WPM] [MESSAGE]
```

| Arg     | Default               | Notes                                  |
|---------|-----------------------|----------------------------------------|
| PORT    | `/dev/ttyUSB1`        | Your keyer's serial port (`pio device list`). |
| WPM     | `25`                  | 5–60.                                  |
| MESSAGE | `CQ TEST DE ATOMKEY ` | Printable ASCII; keyed as Morse.       |

### Examples

```bash
# Default message at 25 WPM
python3 tools/wktest.py /dev/ttyUSB1 25 "CQ TEST DE ATOMKEY"

# Slow enough to read the LED by eye; PARIS is the standard timing word
python3 tools/wktest.py /dev/ttyUSB1 8 "PARIS PARIS"
```

### Expected output

```
host-open OK  -> revision 0x17 (23)
set speed     -> 25 WPM
sending       -> 'CQ TEST DE ATOMKEY'  (watch the LED / listen to the buzzer)
status bytes  -> C4 C0
done.
```

- **`revision 0x17 (23)`** — handshake succeeded; this is the byte a host uses
  to detect the keyer (WK2 v2.3).
- **`C4`** = busy, **`C0`** = idle. At low WPM a long message may still be
  sending when the script's status-drain window (~12 s) closes, so you may see
  only `C4` — that's expected, not an error.

### Notes

- WinKey serial is **1200 baud, 8N1**.
- Opening the port toggles DTR/RTS, which **reboots the ESP32** — the script
  sleeps 2 s after opening to let it boot before the handshake. Expect a short
  delay on connect.

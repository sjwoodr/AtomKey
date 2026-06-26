# AtomKey wiring

How to wire the sidetone buzzer, the CW keying output, and the PTT output to an
M5Stack Atom Lite. Pin assignments here are authoritative because they are the
`#define`s the firmware actually toggles (`src/main.cpp`).

Everything you wire lives on the **2.54 mm side headers**, so the whole build
uses ordinary **Dupont jumper wires** - no Grove cable needed, and the Grove
port is left free.

> Safety first: never wire a rig's KEY (or PTT) line straight to a 3.3 V GPIO.
> Put an optocoupler (preferred) or a transistor between the Atom and the rig,
> as shown below. The bare-GPIO outputs are fine for the LED and a buzzer, and
> harmless if left unconnected.

## Pin map

| Function       | GPIO | Where it lives            | Notes                                   |
|----------------|------|---------------------------|-----------------------------------------|
| RGB status LED | 27   | built-in                  | amber = key down, dim blue = idle/open  |
| Button         | 39   | built-in                  | press = send a self-test message        |
| Sidetone       | 22   | side header               | passive piezo buzzer via series resistor|
| Key out        | 23   | side header               | CW key line, active-high                 |
| PTT out        | 25   | side header               | amp/QSK sequencing, active-high          |
| 5V / 3.3V / GND| -    | side header               | power / ground                          |

> Confirm `G22`, `G23`, `G25` against the silkscreen on your Atom Lite before
> wiring - M5Stack has shuffled the header layout across board revisions. If a
> pin isn't on your board, it's a one-line `#define` change in `src/main.cpp`
> (any free header GPIO - G19, G21, G22, G23, G25, G33 - works; avoid the
> input-only and strapping pins).

## Connectors

The side headers are standard **2.54 mm (0.1") female sockets**. Ordinary
**Dupont jumper wires** plug straight in (male end into the socket). Grab `5V`
and `GND` from these same headers. The Grove port (the keyed 2.0 mm connector)
is unused by this build.

## Sidetone buzzer (G22)

A passive piezo element driven by the ESP32's LEDC square wave. The series
resistor limits the current spike the GPIO sees on each edge (a piezo looks
capacitive) and sets volume - 100-330 ohm is the useful range; smaller is
louder.

```
   Atom Lite                              Passive piezo buzzer
   (side header)                          (ceramic, no driver chip)
   ┌───────────┐                                ┌───┐
   │  G22  ●────┼────[ R 100-330Ω ]─────────────┤ + │
   │           │                                │   │  ))) tone
   │  GND  ●────┼────────────────────────────────┤ - │
   └───────────┘                                └───┘
```

- Passive piezo (not "active" - an active buzzer self-oscillates at a fixed
  pitch and would ignore the keyer's sidetone frequency).
- The element is not really polarised; lead orientation does not matter.

## CW keying output (G23) - preferred: PC817 optocoupler

The optocoupler galvanically isolates the Atom from the rig (no shared ground,
no ground loops, survives voltage surprises). G23 drives the opto's LED; the
opto's phototransistor is a clean isolated "contact" placed across the rig's
KEY tip and ground - exactly like a straight key's contacts.

```
   Atom Lite              PC817 (DIP-4)                 Rig KEY jack
   ┌───────────┐        ┌──────────────┐
   │ G23  ●─────┼──[220Ω]──►|1      4├──────────────────● KEY tip
   │           │        │   LED        │ collector
   │ GND  ●─────┼────────┤2      3├──────────────────● KEY sleeve (gnd)
   └───────────┘        └──────────────┘ emitter
                         (input ‖ isolated output)
```

PC817 pinout: **1 = LED anode, 2 = LED cathode, 3 = emitter, 4 = collector**
(find pin 1 by the dot/notch).

- **R = 220 ohm** from G23 to pin 1. At 3.3 V that is ~10 mA of LED drive -
  solidly on. With the "C" bin (CTR >= 50%) the output can sink at least ~5 mA,
  more than a modern key line needs. If a rig ever keys weakly, drop toward
  150 ohm for more LED current.
- When G23 goes high the phototransistor conducts and pulls KEY to ground
  (~0.1 V) = key-down. G23 low = output open = key-up.
- **Modern solid-state rigs only.** PC817 Vceo is ~35 V; positive keying at
  3-5 V has huge margin. Do **not** use on old tube rigs with high-voltage or
  negative grid-block keying.
- For true full-break-in **QSK**, a faster opto (`6N137`) is better; PC817 is
  fine for normal keying at any WPM.

## CW keying output (G23) - alternative: 2N3904 transistor

If you have a known modern rig sharing a common ground and don't need isolation,
a small-signal NPN low-side switch also works. (Equivalents: 2N2222 / PN2222 /
BC547.)

```
   Atom Lite                                   Rig KEY jack
   ┌───────────┐
   │ G23  ●──[ R1 1kΩ ]──┬── B                ● KEY tip
   │           │         │   2N3904           │
   │           │  [R2 10kΩ]  C ───────────────┘
   │ GND  ●─────┼─────────┴── E ───────────────● KEY sleeve (gnd)
   └───────────┘
```

- **R1 = 1 kohm** G23 -> base (limits base current, saturates the transistor).
- **R2 = 10 kohm** base -> GND, holds the transistor **off** while G23 floats
  during the ESP32's boot/reset, preventing a spurious key-down on power-up.
- Emitter -> GND shared with the rig; collector -> KEY tip.
- Same voltage caveat: Vceo 40 V, so modern positive-keying rigs only.

## PTT output (G25)

PTT uses the identical isolation stage as the key line - just on G25 instead of
G23. Wire a **second PC817** (or 2N3904) the same way, with G25 driving the LED
(or base) and the output across the rig's PTT/amp-keying line and ground. The
firmware asserts PTT before the first element (lead-in) and holds it through the
tail after the last element.

## Shopping list

- Passive piezo buzzer element
- **PC817C** optocouplers (DIP-4) - one for KEY, one for PTT (+ spares)
- Resistors: 220 ohm (opto LED) x2; or for the transistor route, 1 kohm +
  10 kohm + a 2N3904 per line
- **Dupont jumper wires** (male-to-female and male-to-male) for all signals +
  power on the 2.54 mm headers
- A small proto/perf board to hold the optos + resistors (optional but tidy)

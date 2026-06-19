# AtomKey TODO

Remaining work to take AtomKey from "blinks Morse on the bench" (M0–M2, done)
to "a contester can't tell it from a real K1EL WinKeyer."

**Guiding principle:** the host drives the roadmap. The CLX `WinKeyerClient`
will send exactly the commands it needs — implement those first, in the order
the host actually exercises them, and validate against a real K1EL WinKeyer
(the contester's) for *feel*, not just byte correctness. Everything below maps
to the [K1EL WinKey datasheet](https://www.hamcrafters2.com/) (WK2/WK3 host
mode); verify command numbers/params there before implementing.

Status of the M2 baseline lives in [README.md](README.md). Command parsing
scaffolding (admin handler, `PARAM_LEN` table, send queue, non-blocking sender)
is in `src/main.cpp`.

---

## M3 — Robustness & feel (makes it sound like a real keyer)

These are mostly host-driven and bench-verifiable (LED/buzzer/scope).

- [ ] **Weighting** (`0x03`) — adjust dit/space ratio; currently fixed at PARIS.
- [ ] **Dit/dah ratio** (`0x17`) — non-3:1 ratios some ops prefer.
- [ ] **PTT lead-in / tail timing** (`0x04`) — drive the PTT output pin: assert
      PTT, wait lead-in, then key; hold PTT through the tail after the last
      element. Needed for amp/QSK sequencing. (Define a `PTT_OUT_PIN`.)
- [ ] **Buffered speed change** (`+`/`-` and the buffered-speed command in the
      `0x18`–`0x1F` range) — change WPM mid-message without flushing the buffer.
- [ ] **Buffered PTT / timed key-down** (`0x19`–`0x1F` family) — for inserting
      tuning carriers / spacing in a message stream.
- [ ] **Pause** (`0x06`) and **key-immediate** (`0x0B`) — stop/resume sending;
      assert a continuous key-down (tune).
- [ ] **Backspace** (`0x08`) — remove the last char still in the send buffer.
- [ ] **Fuller status byte semantics** — currently only busy/idle (`0xC4`/`0xC0`).
      Add XOFF / buffer-full backpressure so the host throttles, plus the
      breakin and pushbutton status bits per the datasheet.
- [ ] **Serial echo mode** — echo each character back as it is keyed (some
      hosts display sent text from the echo stream).
- [ ] **WinKey mode register** (`0x0E`) — honor autospace, contest (word)
      spacing, paddle-watchdog, key-mode bits.
- [ ] **Farnsworth spacing** (`0x0D`).
- [ ] **Sidetone control** (`0x01`) — enable/disable + pitch from the host
      (right now pitch is the hardcoded `SIDETONE_HZ`).
- [ ] **Pin config** (`0x09`), **first extension** (`0x10`), **key compensation**
      (`0x11`) — honor or explicitly no-op with a comment.
- [ ] **Load defaults** (`0x0F`, 15 bytes) — apply the standard parameter block.
- [ ] **Prosign / merged-letter support** — send concatenated characters with
      no inter-char gap (e.g. AR, SK, BT).

## M4 — Paddle input & local operation

Requires physical paddle wiring; independent of the host-driven keying path.

- [ ] **Paddle inputs** — two GPIOs (dit/dah) with debounce; define pins on the
      Grove/header. (`KEY_OUT_PIN`/`SIDETONE_PIN` already reserved.)
- [ ] **Iambic A/B + Ultimatic** keying modes; honor the mode register.
- [ ] **Paddle echo to host** — report paddle-sent characters so the logger can
      display them.
- [ ] **Paddle switchpoint** (`0x12`) — dit/dah memory timing.
- [ ] **Tune / key-down on button hold** — long-press the Atom button for a
      continuous carrier (handy with no paddle attached).
- [ ] **Speed pot** (`0x05` setup, `0x07` get) — note: Atom Lite has no pot;
      either skip, emulate via button, or add an external pot on an ADC pin.

## Infra / quality (cross-cutting)

- [ ] **Hardware-timer element generation pinned to core 1** — replace the
      `millis()` loop for tight jitter at high WPM (see README "Notes").
- [ ] **`micros()` timing** — sub-ms accuracy; `millis()` granularity hurts
      above ~40 WPM.
- [ ] **Persist settings** (WPM, sidetone, mode) in NVS/`Preferences` across
      reboot.
- [ ] **Keying output stage** — document + build the optocoupler/transistor
      between `KEY_OUT_PIN` and a rig's KEY line; test with a real rig.
- [ ] **Validate against a real K1EL WinKeyer** — A/B the timing/feel with the
      contester; this is the real "done" bar, not byte parity.
- [ ] **Extend `tools/wktest.py`** as new commands land (weighting, PTT, etc.)
      so each feature has a host-side poke.

---

## Done

- [x] **M0** — toolchain / boot (LED).
- [x] **M1** — host-open handshake returns revision `0x17` (verified over wire).
- [x] **M2** — set-WPM, ASCII→Morse via non-blocking sender, clear/abort,
      busy/idle status (PARIS verified on hardware).

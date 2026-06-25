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

- [x] **Weighting** (`0x03`) — adjust dit/space ratio; 10–90, 50 = normal.
- [x] **Dit/dah ratio** (`0x17`) — 33–66, dah/dit = 3·(n/50).
- [x] **PTT lead-in / tail timing** (`0x04`) — drives `PTT_OUT_PIN` (GPIO 25):
      asserts PTT, waits lead-in, keys, then holds PTT for 3 dits + tail after
      the last element. Lead/tail bytes are 10 ms units.
- [ ] **Buffered speed change** (the buffered-speed command in the `0x1C`/`0x1E`
      range) — change WPM mid-message without flushing the buffer. *(Not the
      `+`/`-` chars — those are prosigns AR/DU, not speed steps; see TODO note.)*
- [ ] **Buffered PTT / timed key-down** (`0x18`/`0x19`/`0x1A` family) — for
      inserting tuning carriers / spacing in a message stream. *(Param counts
      now correct in `PARAM_LEN`, so these stay in sync while unimplemented.
      Timed key-down/wait is also what lights the status WAIT bit.)*
- [ ] **Autospace** (mode `0x0E` bit 1) — insert a word space automatically;
      host-relevant, left out of the mode-register pass above.
- [ ] **`0x1D` dual meaning** — the WK2 datasheet lists it as both buffered HSCW
      speed *and* port-select; WK3 resolves it as port-select. Currently
      `PARAM_LEN = 1` (stays in sync either way); pick a meaning if ever used.
- [x] **Pause** (`0x06`) and **key-immediate** (`0x0B`) — pause holds at the
      next char boundary; key-immediate asserts a continuous key-down (tune).
- [x] **Backspace** (`0x08`) — removes the last char still in the send buffer.
- [x] **Fuller status byte semantics** — busy/idle plus XOFF (`bit 0`,
      buffer > 2/3 full) so the host throttles, emitted on every status edge.
      *(WAIT/BREAKIN/pushbutton bits await timed-buffered + paddle support.)*
- [x] **Serial echo mode** — mode bit 2 echoes each char back as it is keyed.
- [x] **WinKey mode register** (`0x0E`) — honors contest (word) spacing and
      serial echo. Autospace (bit 1) broken out as its own item below; paddle
      bits (swap, paddle-echo, watchdog, key-mode) stored for M4.
- [x] **Farnsworth spacing** (`0x0D`) — elements at Farnsworth WPM, char/word
      spacing at the (slower) main speed; auto-off when main ≥ Farnsworth.
- [x] **Sidetone control** (`0x01`) — pitch from the host (divisor index N) plus
      the paddle-only mute bit.
- [x] **Key compensation** (`0x11`) — fixed ms added to every element.
      **First extension** (`0x10`) stored (T/R timing deferred). **Pin config**
      (`0x09`) consumed/no-op with comment.
- [x] **Load defaults** (`0x0F`, 15 bytes) — applies the standard parameter
      block in datasheet order (honored fields; pot/paddle bytes skipped).
- [x] **Prosign / merged-letter support** — merge command (`0x1B`) concatenates
      two chars with no inter-char gap (e.g. AR, SK, BT).

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

// AtomKey - a WinKey-protocol-compatible CW keyer for the M5Stack Atom Lite.
//
// Purpose: a cheap bench device that speaks enough of the K1EL WinKey host
// protocol for a logging program (e.g. ContestLogX's WinKeyerClient) to talk
// to it as if it were a real WinKeyer. Element timing is generated on the
// ESP32, not the host PC, which is the whole point of offloading CW keying.
//
// Hardware (Atom Lite):
//   - RGB LED (SK6812) on GPIO 27  -> visual key-down feedback + status
//   - Button on GPIO 39 (input-only, external pull-up) -> self-test
//   - Side header (2.54 mm): GPIO 22 sidetone, GPIO 23 key-out, GPIO 25 PTT
//
// Everything user-wired lives on the 2.54 mm side headers, so the whole build
// uses ordinary Dupont jumpers and the Grove port is left free. No rig
// required: attach a passive piezo buzzer between SIDETONE_PIN and GND to hear
// the CW. KEY_OUT_PIN and PTT_OUT_PIN are driven active-high for a future
// optocoupler/transistor stage, but are harmless if left unconnected.
//
// WiFi/BT are never started, so nothing competes with the keying loop for the
// CPU. For production-grade jitter you'd move element timing onto a hardware
// timer pinned to core 1; millis() is fine for a bench test device.

#include <Arduino.h>
#include <FastLED.h>
#include <esp_log.h>

// ----------------------------------------------------------------------------
// Pin map (Atom Lite)
// ----------------------------------------------------------------------------
#define LED_PIN       27   // built-in SK6812
#define LED_COUNT     1
#define BTN_PIN       39   // built-in button (input-only, no internal pull)
#define SIDETONE_PIN  22   // side header -> passive piezo buzzer
#define KEY_OUT_PIN   23   // side header -> opto/transistor to rig KEY (active-high)
#define PTT_OUT_PIN   25   // side header -> PTT to amp/QSK sequencer (active-high)

#define SIDETONE_HZ   700  // default sidetone pitch (host can override via 0x01)
#define WK_REVISION   23   // reported on host-open (0x17 = WK2 v2.3)

// ----------------------------------------------------------------------------
// LED
// ----------------------------------------------------------------------------
static CRGB leds[LED_COUNT];

static const CRGB COLOR_BOOT     = CRGB(40, 0, 40);   // purple  - powered, not open
static const CRGB COLOR_IDLE     = CRGB(0, 0, 25);    // dim blue - host open, idle
static const CRGB COLOR_KEYDOWN  = CRGB(220, 120, 0); // amber   - keying
static const CRGB COLOR_OPEN     = CRGB(0, 80, 0);    // green   - host-open ack

static void setLed(const CRGB &c) {
  leds[0] = c;
  FastLED.show();
}

// ----------------------------------------------------------------------------
// Keying primitives
// ----------------------------------------------------------------------------
static bool g_hostOpen = false;

// Sidetone pitch is host-settable (0x01). Bit 7 of that command means
// "paddle-only sidetone" — mute the tone for host-sourced CW. We have no
// paddle yet, so paddle-only simply silences the buzzer for sent text.
static uint16_t g_sidetoneHz        = SIDETONE_HZ;
static bool     g_sidetonePaddleOnly = false;

// WK2 sidetone command encodes pitch as a divisor index N (bits 3..0), not Hz.
static uint16_t sidetoneHzFor(uint8_t n) {
  switch (n) {
    case 1:  return 4000; case 2: return 2000; case 3: return 1333;
    case 4:  return 1000; case 5: return 800;  case 6: return 666;
    case 7:  return 571;  case 8: return 500;  case 9: return 444;
    case 10: return 400;
    default: return 0;                  // 0/invalid -> leave pitch unchanged
  }
}

// Sidetone via the ESP32 LEDC peripheral. Arduino's tone() on some core
// versions does not attach the pin first, so it errors ("LEDC is not
// initialized") and the ESP-IDF logger dumps that text onto UART0 — the same
// line carrying the WinKey protocol — corrupting the byte stream for the host.
// Drive LEDC directly instead, guarding for the 2.x/3.x API change.
#define SIDETONE_LEDC_CH 0

static void sidetoneInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(SIDETONE_PIN, SIDETONE_HZ, 10);
#else
  ledcSetup(SIDETONE_LEDC_CH, SIDETONE_HZ, 10);
  ledcAttachPin(SIDETONE_PIN, SIDETONE_LEDC_CH);
#endif
}

static void sidetoneOn() {
  if (g_sidetonePaddleOnly) return;     // muted for host-sourced CW
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(SIDETONE_PIN, g_sidetoneHz);
#else
  ledcWriteTone(SIDETONE_LEDC_CH, g_sidetoneHz);
#endif
}

static void sidetoneOff() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(SIDETONE_PIN, 0);
#else
  ledcWriteTone(SIDETONE_LEDC_CH, 0);
#endif
}

static void pttOn()  { digitalWrite(PTT_OUT_PIN, HIGH); }
static void pttOff() { digitalWrite(PTT_OUT_PIN, LOW); }

static void keyDown() {
  sidetoneOn();
  digitalWrite(KEY_OUT_PIN, HIGH);
  setLed(COLOR_KEYDOWN);
}

static void keyUp() {
  sidetoneOff();
  digitalWrite(KEY_OUT_PIN, LOW);
  setLed(g_hostOpen ? COLOR_IDLE : COLOR_BOOT);
}

// ----------------------------------------------------------------------------
// Morse table
// ----------------------------------------------------------------------------
static const char *morseFor(char c) {
  switch (toupper((unsigned char)c)) {
    case 'A': return ".-";    case 'B': return "-...";  case 'C': return "-.-.";
    case 'D': return "-..";   case 'E': return ".";     case 'F': return "..-.";
    case 'G': return "--.";   case 'H': return "....";  case 'I': return "..";
    case 'J': return ".---";  case 'K': return "-.-";   case 'L': return ".-..";
    case 'M': return "--";    case 'N': return "-.";    case 'O': return "---";
    case 'P': return ".--.";  case 'Q': return "--.-";  case 'R': return ".-.";
    case 'S': return "...";   case 'T': return "-";     case 'U': return "..-";
    case 'V': return "...-";  case 'W': return ".--";   case 'X': return "-..-";
    case 'Y': return "-.--";  case 'Z': return "--..";
    case '0': return "-----"; case '1': return ".----"; case '2': return "..---";
    case '3': return "...--"; case '4': return "....-"; case '5': return ".....";
    case '6': return "-...."; case '7': return "--..."; case '8': return "---..";
    case '9': return "----.";
    case '.': return ".-.-.-"; case ',': return "--..--"; case '?': return "..--..";
    case '/': return "-..-.";  case '=': return "-...-";  case '+': return ".-.-.";
    case '-': return "-....-"; case ':': return "---..."; case '(': return "-.--.";
    case ')': return "-.--.-"; case '"': return ".-..-."; case '@': return ".--.-.";
    case '\'': return ".----.";
    default:  return nullptr;
  }
}

// ----------------------------------------------------------------------------
// Send queue (ring buffer of characters waiting to be keyed)
// ----------------------------------------------------------------------------
#define QUEUE_SIZE 256

// Queued bytes are ASCII (>= 0x20). Bit 7 is a private flag meaning "run
// straight into the next character with no inter-character gap" — used to
// build prosigns from the merge command (0x1B). It is masked off before the
// Morse lookup, so it never reaches morseFor().
#define MERGE_FLAG 0x80

static char     g_queue[QUEUE_SIZE];
static volatile uint16_t g_qHead = 0, g_qTail = 0;

static bool     qEmpty() { return g_qHead == g_qTail; }
static uint16_t qFill()  { return (g_qTail - g_qHead + QUEUE_SIZE) % QUEUE_SIZE; }
static void qPush(char c) {
  uint16_t next = (g_qTail + 1) % QUEUE_SIZE;
  if (next != g_qHead) { g_queue[g_qTail] = c; g_qTail = next; }
}
static char qPop() {
  char c = g_queue[g_qHead];
  g_qHead = (g_qHead + 1) % QUEUE_SIZE;
  return c;
}
static void qClear() { g_qHead = g_qTail = 0; }
// Backspace (0x08): drop the most recently buffered, not-yet-keyed character.
static void qBackspace() {
  if (g_qTail != g_qHead) g_qTail = (g_qTail - 1 + QUEUE_SIZE) % QUEUE_SIZE;
}

// ----------------------------------------------------------------------------
// Timing - PARIS standard: dit(ms) = 1200 / WPM, then adjusted for weighting,
// dit/dah ratio, Farnsworth spacing and key compensation.
//
// Derived element/gap durations are precomputed in recompute() whenever any
// input changes, so sendTick() stays a cheap table lookup.
// ----------------------------------------------------------------------------
static uint8_t  g_wpm     = 25;   // 5..99
static uint8_t  g_weight  = 50;   // 10..90, 50 = normal (0x03)
static uint8_t  g_ratio   = 50;   // 33..66, dah/dit = 3*(n/50)  (0x17)
static uint8_t  g_farns   = 0;    // 0 = off, else 10..99 Farnsworth WPM (0x0D)
static uint8_t  g_keyComp = 0;    // ms added to every element  (0x11)
static uint8_t  g_mode    = 0;    // WinKey mode register        (0x0E)

static uint32_t g_ditMs   = 1200 / 25;     // spacing-dit (main speed)
static uint32_t g_ditOn, g_dahOn;          // keyed element on-times
static uint32_t g_elemGap;                 // intra-character (1-dit) gap
static uint32_t g_charGap;                 // remainder to a 3-dit char gap
static uint32_t g_wordGap;                 // remainder to a 7- (or 6-) dit word gap

// PTT lead-in / tail (0x04), each a raw byte in 10 ms units.
static uint8_t  g_pttLead = 0;
static uint8_t  g_pttTail = 0;
static uint8_t  g_firstExt = 0;            // 0x10 first extension (stored, no-op)

// Transient control state.
static bool g_paused    = false;           // 0x06 pause (holds at char boundary)
static bool g_keyDown   = false;           // 0x0B key-immediate / tune
static bool g_pttActive = false;           // PTT currently asserted for a session
static bool g_serialEcho = false;          // mode bit 2: echo keyed chars to host

static uint32_t clampMs(long v) { return v < 1 ? 1 : (uint32_t)v; }

static void recompute() {
  if (g_wpm < 5)  g_wpm = 5;
  if (g_wpm > 99) g_wpm = 99;
  g_ditMs = 1200 / g_wpm;

  // Farnsworth: elements keyed at the (faster) Farnsworth speed, but character
  // and word spacing stay at the slower main speed. Auto-off once main >= farns.
  bool     farns  = (g_farns >= 10 && g_farns > g_wpm);
  uint32_t spcDit = g_ditMs;
  uint32_t eltDit = farns ? (1200 / g_farns) : g_ditMs;

  // Weighting shifts time from the inter-element gap into the elements (or vice
  // versa) while holding each element's cycle length constant, so speed is
  // unchanged. Key compensation adds a fixed, speed-independent ms on top.
  int  adj  = ((int)g_weight - 50) * (int)eltDit / 100;
  int  comp = (int)g_keyComp;
  long dahBase = (long)eltDit * 3 * g_ratio / 50;
  bool ct = g_mode & 0x01;                 // contest spacing: 6-dit word gap

  g_ditOn   = clampMs((long)eltDit + adj + comp);
  g_dahOn   = clampMs(dahBase + adj + comp);
  g_elemGap = clampMs((long)eltDit - adj - comp);
  g_charGap = clampMs(3L * spcDit - (long)g_elemGap);          // -> 3-dit total
  g_wordGap = clampMs(((ct ? 6L : 7L) - 3L) * (long)spcDit);   // -> 7- (or 6-) dit
}

// ----------------------------------------------------------------------------
// Non-blocking element sender
//
// Phases per session: optional PTT lead-in, then per character an ON element
// followed by the intra-character gap, an inter-character gap after the last
// element, and a wider word gap for a space — all durations from recompute()
// so weighting/ratio/Farnsworth apply. A merge (prosign) skips the char gap; a
// space-less idle holds PTT through the tail. Keeping this non-blocking means
// an abort (0x0A) stops keying within one loop() pass.
// ----------------------------------------------------------------------------
enum Phase { P_IDLE, P_PTT_LEAD, P_ELEM_ON, P_ELEM_GAP, P_CHAR_GAP, P_WORD_GAP, P_PTT_TAIL };

static Phase       g_phase     = P_IDLE;
static uint32_t    g_phaseEnd  = 0;
static const char *g_pattern   = nullptr;
static uint8_t     g_patPos    = 0;
static char        g_curChar   = 0;        // char being keyed (for serial echo)
static bool        g_mergeNext = false;    // current char runs into the next (prosign)

static void startElement() {
  bool dah = (g_pattern[g_patPos] == '-');
  keyDown();
  g_phase    = P_ELEM_ON;
  g_phaseEnd = millis() + (dah ? g_dahOn : g_ditOn);
}

// End-of-spacing handler shared by char- and word-gap: queue more text -> keep
// going; otherwise hold PTT for the tail before dropping it.
static void afterSpacing(uint32_t now) {
  if (!qEmpty() || g_paused) {
    g_phase = P_IDLE;
  } else if (g_pttActive) {
    g_phase    = P_PTT_TAIL;
    g_phaseEnd = now + 3 * g_ditMs + (uint32_t)g_pttTail * 10;  // 3 dits + tail
  } else {
    g_phase = P_IDLE;
  }
}

static void sendTick() {
  if (g_keyDown) return;                    // tune in progress: sender suspended
  uint32_t now = millis();

  // New text during the PTT tail resumes immediately — PTT is still asserted.
  if (g_phase == P_PTT_TAIL && !qEmpty() && !g_paused) g_phase = P_IDLE;

  if ((int32_t)(now - g_phaseEnd) < 0 && g_phase != P_IDLE) return;

  switch (g_phase) {
    case P_IDLE: {
      if (g_paused || qEmpty()) return;
      // Assert PTT and wait the lead-in before the first element of a session.
      if (!g_pttActive) {
        pttOn();
        g_pttActive = true;
        if (g_pttLead > 0) {
          g_phase    = P_PTT_LEAD;
          g_phaseEnd = now + (uint32_t)g_pttLead * 10;
          return;
        }
      }
      char raw = qPop();
      g_mergeNext = (raw & MERGE_FLAG) != 0;
      g_curChar   = raw & 0x7F;
      if (g_curChar == ' ') {
        if (g_serialEcho) Serial.write(' ');
        g_phase    = P_WORD_GAP;
        g_phaseEnd = now + g_wordGap;
        return;
      }
      const char *m = morseFor(g_curChar);
      if (!m) return;                       // unknown char: drop, try next tick
      g_pattern = m;
      g_patPos  = 0;
      startElement();
      break;
    }
    case P_PTT_LEAD:
      g_phase = P_IDLE;                      // lead elapsed; re-enter to key (PTT up)
      break;

    case P_ELEM_ON:
      keyUp();
      g_phase    = P_ELEM_GAP;
      g_phaseEnd = now + g_elemGap;          // intra-character gap
      break;

    case P_ELEM_GAP:
      g_patPos++;
      if (g_pattern[g_patPos] != '\0') {
        startElement();
      } else {
        if (g_serialEcho) Serial.write(g_curChar);
        if (g_mergeNext) {
          g_phase = P_IDLE;                  // prosign: straight into next char
        } else {
          g_phase    = P_CHAR_GAP;
          g_phaseEnd = now + g_charGap;
        }
      }
      break;

    case P_CHAR_GAP:
    case P_WORD_GAP:
      afterSpacing(now);
      break;

    case P_PTT_TAIL:
      pttOff();
      g_pttActive = false;
      g_phase     = P_IDLE;
      break;
  }
}

// ----------------------------------------------------------------------------
// WinKey status byte (WK2-mode, datasheet Table 11): tag bits 7..5 = 110,
// bit 4 WAIT, bit 3 = 0 (identifies a WK status byte), bit 2 BUSY, bit 1
// BREAKIN, bit 0 XOFF. We drive BUSY (sending) and XOFF (buffer > 2/3 full so
// the host throttles); WAIT/BREAKIN await timed-buffered and paddle support.
// ----------------------------------------------------------------------------
static uint8_t  g_lastStatus = 0xC0;

static uint8_t statusByte() {
  uint8_t st = 0xC0;                                  // 110 0 0 0 0 0
  if (g_phase != P_IDLE || !qEmpty() || g_keyDown) st |= 0x04;   // BUSY
  if (qFill() > (QUEUE_SIZE * 2 / 3))              st |= 0x01;   // XOFF
  return st;
}

static void sendStatus() {
  g_lastStatus = statusByte();
  Serial.write(g_lastStatus);
}

// Notify the host whenever the status byte changes (busy<->idle, XOFF edges).
static void reportStatusEdge() {
  uint8_t st = statusByte();
  if (st != g_lastStatus) {
    Serial.write(st);
    g_lastStatus = st;
  }
}

// ----------------------------------------------------------------------------
// WinKey command parser
//
// Parameter byte counts per command (0x01..0x1F). Buffered commands
// (0x18..0x1F) are best-effort - verify against the WK datasheet. Consuming
// the right number of param bytes is what keeps us from desyncing and keying
// stray config bytes as Morse.
// ----------------------------------------------------------------------------
// Counts verified against the WK2 datasheet v23. NB: 0x16 buffer-pointer is
// variable (the "add nulls" sub-command takes an extra count byte); we treat it
// as 1 here since the host path we target never uses it — revisit if it does.
static const uint8_t PARAM_LEN[0x20] = {
  /*00 admin*/ 0, /*01 sidetone*/ 1, /*02 speed*/ 1, /*03 weight*/ 1,
  /*04 ptt l/t*/ 2, /*05 pot setup*/ 3, /*06 pause*/ 1, /*07 get pot*/ 0,
  /*08 backsp*/ 0, /*09 pin cfg*/ 1, /*0A clear*/ 0, /*0B key imm*/ 1,
  /*0C hscw*/ 1, /*0D farns*/ 1, /*0E wk mode*/ 1, /*0F defaults*/ 15,
  /*10 first ext*/ 1, /*11 key comp*/ 1, /*12 paddle sw*/ 1, /*13 null*/ 0,
  /*14 sw paddle*/ 1, /*15 status*/ 0, /*16 pointer*/ 1, /*17 ratio*/ 1,
  /*18 b-ptt*/ 1, /*19 b-keydown*/ 1, /*1A b-wait*/ 1, /*1B merge*/ 2,
  /*1C b-speed*/ 1, /*1D hscw/port*/ 1, /*1E cancel-spd*/ 0, /*1F b-nop*/ 0,
};

static bool    g_inAdmin   = false;   // saw 0x00, next byte is the admin sub-cmd
static bool    g_adminEcho = false;   // admin echo (0x00 0x04): next byte echoed
static uint8_t g_cmd       = 0;
static int     g_need      = 0;       // param bytes still expected for g_cmd
static uint8_t g_param[16];
static int     g_got       = 0;

// Stop everything: drain the buffer and cancel tune/pause/PTT. Shared by the
// clear-buffer command (0x0A) and the admin reset (0x00 0x01).
static void clearAll() {
  qClear();
  g_keyDown = false;
  g_paused  = false;
  keyUp();
  pttOff();
  g_pttActive = false;
  g_phase = P_IDLE;
}

// Load Defaults (0x0F): 15-byte block in the datasheet's fixed order. We apply
// the parameters we honor and skip the pot/paddle ones we don't.
static void applyDefaults(const uint8_t *p, int n) {
  if (n < 15) return;
  g_mode       = p[0];
  g_serialEcho = (g_mode & 0x04) != 0;
  if (p[1]) g_wpm = p[1];                                   // 0 = leave as-is
  { uint16_t hz = sidetoneHzFor(p[2] & 0x0F);
    g_sidetonePaddleOnly = (p[2] & 0x80) != 0;
    if (hz) g_sidetoneHz = hz; }
  g_weight   = constrain(p[3], 10, 90);
  g_pttLead  = p[4];
  g_pttTail  = p[5];
  // p[6] MinWPM, p[7] WPM range -> speed pot, not present on the Atom Lite.
  g_firstExt = p[8];
  g_keyComp  = p[9];
  g_farns    = p[10];
  // p[11] paddle switchpoint -> paddle (M4).
  g_ratio    = constrain(p[12], 33, 66);
  // p[13] pin config, p[14] don't-care.
  recompute();
}

static void dispatch(uint8_t cmd, const uint8_t *p, int n) {
  switch (cmd) {
    case 0x01: {                        // sidetone: bit7 paddle-only, bits3..0 pitch
      uint16_t hz = sidetoneHzFor(p[0] & 0x0F);
      g_sidetonePaddleOnly = (p[0] & 0x80) != 0;
      if (hz) g_sidetoneHz = hz;
      break;
    }
    case 0x02:                          // set WPM (0 = use speed pot; ignored)
      if (n >= 1 && p[0] != 0) { g_wpm = p[0]; recompute(); }
      break;
    case 0x03:                          // weighting (10..90, 50 = normal)
      g_weight = constrain(p[0], 10, 90); recompute();
      break;
    case 0x04:                          // PTT lead-in / tail (10 ms units)
      g_pttLead = p[0]; g_pttTail = p[1];
      break;
    case 0x06:                          // pause (1) / resume (0)
      g_paused = (p[0] != 0);
      break;
    case 0x08:                          // backspace the input buffer
      qBackspace();
      break;
    case 0x0A:                          // clear buffer / abort
      clearAll();
      break;
    case 0x0B:                          // key immediate: 1 = key down (tune), 0 = up
      if (p[0]) { g_keyDown = true;  pttOn();  g_pttActive = true;  keyDown(); }
      else      { g_keyDown = false; keyUp();  pttOff(); g_pttActive = false; }
      break;
    case 0x0D:                          // Farnsworth WPM (0/<=wpm disables)
      g_farns = p[0]; recompute();
      break;
    case 0x0E:                          // WinKey mode register
      g_mode = p[0]; g_serialEcho = (g_mode & 0x04) != 0; recompute();
      break;
    case 0x0F:                          // load 15-byte defaults block
      applyDefaults(p, n);
      break;
    case 0x10:                          // first extension (stored, not yet honored)
      g_firstExt = p[0];
      break;
    case 0x11:                          // key compensation (ms added to elements)
      g_keyComp = p[0]; recompute();
      break;
    case 0x15:                          // host status request
      sendStatus();
      break;
    case 0x17:                          // dit/dah ratio (33..66, 50 = 1:3)
      g_ratio = constrain(p[0], 33, 66); recompute();
      break;
    case 0x1B:                          // merge letters -> prosign (no inter-char gap)
      qPush((char)(p[0] | MERGE_FLAG));
      qPush((char)p[1]);
      break;
    default:
      // Recognized but not acted on (pot setup 0x05, pin cfg 0x09, HSCW 0x0C,
      // paddle switchpoint 0x12, buffer pointer 0x16, the buffered family
      // 0x18..0x1F bar merge). Params are already consumed, so we stay in sync.
      break;
  }
}

static void handleAdmin(uint8_t sub) {
  switch (sub) {
    case 0x02:                          // host open -> return firmware revision
      g_hostOpen = true;
      Serial.write(WK_REVISION);
      setLed(COLOR_OPEN);
      delay(60);
      keyUp();
      break;
    case 0x03:                          // host close
      g_hostOpen = false;
      keyUp();
      break;
    case 0x01:                          // reset
      clearAll();
      break;
    case 0x04:                          // echo test: next byte is echoed back
      g_adminEcho = true;
      break;
    default:
      break;                            // other admin subs: ignored for now
  }
}

static void parseByte(uint8_t b) {
  if (g_adminEcho)          { Serial.write(b); g_adminEcho = false; return; }
  if (g_need > 0)           { g_param[g_got++] = b; if (--g_need == 0) dispatch(g_cmd, g_param, g_got); return; }
  if (g_inAdmin)            { g_inAdmin = false; handleAdmin(b); return; }
  if (b == 0x00)            { g_inAdmin = true; return; }
  if (b >= 0x20)            { qPush((char)b); return; }   // printable ASCII -> Morse (0x20 = space)

  // Control byte 0x01..0x1F
  g_cmd  = b;
  g_got  = 0;
  g_need = PARAM_LEN[b];
  if (g_need == 0) dispatch(b, nullptr, 0);
}

// ----------------------------------------------------------------------------
// Button self-test: queue a canned message so you can verify keying with no
// host attached. Simple debounce on the falling edge.
// ----------------------------------------------------------------------------
static void serviceButton() {
  static bool     last = HIGH;
  static uint32_t lastChange = 0;
  bool now = digitalRead(BTN_PIN);
  if (now != last && (millis() - lastChange) > 40) {
    lastChange = millis();
    if (now == LOW) {                   // pressed (button to GND)
      const char *msg = "TEST DE ATOMKEY ";
      for (const char *s = msg; *s; ++s) qPush(*s);
    }
    last = now;
  }
}

// ----------------------------------------------------------------------------
void setup() {
  // Silence ESP-IDF logging: it shares UART0 with the WinKey protocol, and any
  // stray log text corrupts the byte stream a host (CLX, QLog, ...) parses.
  esp_log_level_set("*", ESP_LOG_NONE);

  pinMode(KEY_OUT_PIN, OUTPUT);
  digitalWrite(KEY_OUT_PIN, LOW);
  pinMode(PTT_OUT_PIN, OUTPUT);
  digitalWrite(PTT_OUT_PIN, LOW);
  pinMode(BTN_PIN, INPUT);

  sidetoneInit();

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(40);
  setLed(COLOR_BOOT);

  recompute();                          // derive timing from the default params
  Serial.begin(1200);                   // WinKey host protocol, 8N1
}

void loop() {
  while (Serial.available()) parseByte((uint8_t)Serial.read());
  serviceButton();
  sendTick();
  reportStatusEdge();
}

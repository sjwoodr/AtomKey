// AtomKey - a WinKey-protocol-compatible CW keyer for the M5Stack Atom Lite.
//
// Purpose: a ~$10 bench device that speaks enough of the K1EL WinKey host
// protocol for a logging program (e.g. ContestLogX's WinKeyerClient) to talk
// to it as if it were a real WinKeyer. Element timing is generated on the
// ESP32, not the host PC, which is the whole point of offloading CW keying.
//
// Hardware (Atom Lite):
//   - RGB LED (SK6812) on GPIO 27  -> visual key-down feedback + status
//   - Button on GPIO 39 (input-only, external pull-up) -> self-test
//   - Grove port: GPIO 26, GPIO 32 -> sidetone buzzer / future rig keying
//
// No rig required: attach a passive piezo buzzer between SIDETONE_PIN and GND
// to hear the CW. KEY_OUT_PIN is driven too (active-high) for a future
// optocoupler/transistor stage, but is harmless if left unconnected.
//
// WiFi/BT are never started, so nothing competes with the keying loop for the
// CPU. For production-grade jitter you'd move element timing onto a hardware
// timer pinned to core 1; millis() is fine for a bench test device.

#include <Arduino.h>
#include <FastLED.h>

// ----------------------------------------------------------------------------
// Pin map (Atom Lite)
// ----------------------------------------------------------------------------
#define LED_PIN       27   // built-in SK6812
#define LED_COUNT     1
#define BTN_PIN       39   // built-in button (input-only, no internal pull)
#define SIDETONE_PIN  26   // Grove pin 1 -> passive piezo buzzer
#define KEY_OUT_PIN   32   // Grove pin 2 -> future opto/transistor to rig KEY

#define SIDETONE_HZ   700  // contest-typical sidetone pitch
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

static void keyDown() {
  tone(SIDETONE_PIN, SIDETONE_HZ);
  digitalWrite(KEY_OUT_PIN, HIGH);
  setLed(COLOR_KEYDOWN);
}

static void keyUp() {
  noTone(SIDETONE_PIN);
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
static char     g_queue[QUEUE_SIZE];
static volatile uint16_t g_qHead = 0, g_qTail = 0;

static bool qEmpty()  { return g_qHead == g_qTail; }
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

// ----------------------------------------------------------------------------
// Timing - PARIS standard: dit(ms) = 1200 / WPM
// ----------------------------------------------------------------------------
static uint8_t  g_wpm   = 25;
static uint32_t g_ditMs = 1200 / 25;

static void setWpm(uint8_t wpm) {
  if (wpm < 5)  wpm = 5;
  if (wpm > 60) wpm = 60;
  g_wpm   = wpm;
  g_ditMs = 1200 / wpm;
}

// ----------------------------------------------------------------------------
// Non-blocking element sender
//
// Per character: ON element, then 1-dit gap between elements; after the last
// element a 3-dit inter-character gap. A space adds a further 4 dits so the
// total word gap is 7 dits. Keeping this non-blocking means an abort (0x0A)
// stops keying within one loop() pass.
// ----------------------------------------------------------------------------
enum Phase { P_IDLE, P_ELEM_ON, P_ELEM_GAP, P_CHAR_GAP, P_WORD_GAP };

static Phase       g_phase     = P_IDLE;
static uint32_t    g_phaseEnd  = 0;
static const char *g_pattern   = nullptr;
static uint8_t     g_patPos    = 0;
static bool        g_wasBusy   = false;

static void startElement() {
  bool dah = (g_pattern[g_patPos] == '-');
  keyDown();
  g_phase    = P_ELEM_ON;
  g_phaseEnd = millis() + (dah ? 3 * g_ditMs : g_ditMs);
}

static void sendTick() {
  uint32_t now = millis();
  if ((int32_t)(now - g_phaseEnd) < 0 && g_phase != P_IDLE) return;

  switch (g_phase) {
    case P_IDLE: {
      if (qEmpty()) return;
      char c = qPop();
      if (c == ' ') {
        g_phase    = P_WORD_GAP;
        g_phaseEnd = now + 4 * g_ditMs;   // +3-dit char gap already given = 7 total
        return;
      }
      const char *m = morseFor(c);
      if (!m) return;                      // unknown char: drop, try next on next tick
      g_pattern = m;
      g_patPos  = 0;
      startElement();
      break;
    }
    case P_ELEM_ON:
      keyUp();
      g_phase    = P_ELEM_GAP;
      g_phaseEnd = now + g_ditMs;          // 1-dit intra-character gap
      break;

    case P_ELEM_GAP:
      g_patPos++;
      if (g_pattern[g_patPos] != '\0') {
        startElement();
      } else {
        g_phase    = P_CHAR_GAP;
        g_phaseEnd = now + 2 * g_ditMs;    // +1 already elapsed = 3-dit char gap
      }
      break;

    case P_CHAR_GAP:
    case P_WORD_GAP:
      g_phase = P_IDLE;
      break;
  }
}

// ----------------------------------------------------------------------------
// WinKey status byte (simplified). Real WK status: bits 7,6 = 1; bit 2 = BUSY.
// Verify the full bit map against the K1EL WK datasheet before trusting it.
// ----------------------------------------------------------------------------
static void sendStatus() {
  bool busy = !qEmpty() || g_phase != P_IDLE;
  Serial.write(busy ? 0xC4 : 0xC0);
}

static void reportBusyEdge() {
  bool busy = !qEmpty() || g_phase != P_IDLE;
  if (busy != g_wasBusy) {
    Serial.write(busy ? 0xC4 : 0xC0);     // notify host on busy<->idle transition
    g_wasBusy = busy;
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
static const uint8_t PARAM_LEN[0x20] = {
  /*00 admin*/ 0, /*01 sidetone*/ 1, /*02 speed*/ 1, /*03 weight*/ 1,
  /*04 ptt l/t*/ 2, /*05 pot setup*/ 3, /*06 pause*/ 1, /*07 get pot*/ 0,
  /*08 backsp*/ 0, /*09 pin cfg*/ 1, /*0A clear*/ 0, /*0B key imm*/ 1,
  /*0C hscw*/ 1, /*0D farns*/ 1, /*0E wk mode*/ 1, /*0F defaults*/ 15,
  /*10 first ext*/ 1, /*11 key comp*/ 1, /*12 paddle sw*/ 1, /*13 null*/ 0,
  /*14 sw paddle*/ 1, /*15 status*/ 0, /*16 pointer*/ 1, /*17 ratio*/ 1,
  /*18*/ 1, /*19*/ 1, /*1A*/ 1, /*1B*/ 1, /*1C*/ 1, /*1D*/ 0, /*1E*/ 0, /*1F*/ 1,
};

static bool    g_inAdmin   = false;   // saw 0x00, next byte is the admin sub-cmd
static bool    g_adminEcho = false;   // admin echo (0x00 0x04): next byte echoed
static uint8_t g_cmd       = 0;
static int     g_need      = 0;       // param bytes still expected for g_cmd
static uint8_t g_param[16];
static int     g_got       = 0;

static void dispatch(uint8_t cmd, const uint8_t *p, int n) {
  switch (cmd) {
    case 0x02:                          // set WPM (0 = use speed pot; ignored)
      if (n >= 1 && p[0] != 0) setWpm(p[0]);
      break;
    case 0x0A:                          // clear buffer / abort
      qClear();
      keyUp();
      g_phase = P_IDLE;
      break;
    case 0x15:                          // host status request
      sendStatus();
      break;
    default:
      // Recognized but not acted on yet (sidetone, weighting, pin cfg, ...).
      // Params have already been consumed, so we stay in sync.
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
      qClear();
      keyUp();
      g_phase = P_IDLE;
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
  pinMode(KEY_OUT_PIN, OUTPUT);
  digitalWrite(KEY_OUT_PIN, LOW);
  pinMode(BTN_PIN, INPUT);

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(40);
  setLed(COLOR_BOOT);

  setWpm(25);
  Serial.begin(1200);                   // WinKey host protocol, 8N1
}

void loop() {
  while (Serial.available()) parseByte((uint8_t)Serial.read());
  serviceButton();
  sendTick();
  reportBusyEdge();
}

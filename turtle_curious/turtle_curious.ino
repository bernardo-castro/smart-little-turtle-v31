// ============================================================================
// Keyestudio Smart Little Turtle V3.1
// Cara Viva Permanente + Transiciones Suaves + Seguimiento de Mano + Cola BLE
// ============================================================================
// 1) CARA VIVA SINCRONIZADA 1:1 CON EL ÁNGULO DEL CUELLO + SEGUIMIENTO DE MANO:
//    - Cara siempre activa en brillo 8 (motores bloqueados en LOW).
//    - Posición horizontal de la cara (7 zonas: -3..-2..-1..0..+1..+2..+3)
//      sincronizada en tiempo real con el ángulo exacto del servo ultrasónico:
//      si cruza por el centro pasa por el centro, y si queda mirando a un lado
//      permanece mirando exactamente hacia ese lado mientras sigue pestañeando.
//    - Parpadeo orgánico multi-etapa (párpado baja y sube fila por fila cada
//      2.6~5.6 s, con doble pestañeo ocasional) y giro amortiguado (5~30 s).
//    - Al acercar la mano (<= 12 cm): sigue tu mano con el cuello y desplaza
//      la cara sincronizada con el ángulo real; al retirarla sonríe "^_^"
//      (sin guiñar) y reabre suavemente sus ojos en el ángulo actual.
//    - Si levantas el robot de la mesa: pone cara "O_o" / "o_O" al instante.
//
// 2) COLA AGRUPADA DE NOTIFICACIONES BLUETOOTH DX-BT24 (UART 9600 bps):
//    - Íconos en brillo 15; textos y gestos en brillo 8:
//        "M:<Remitente>\n"        -> EMAIL (✉) agrupado por remitente (xN)
//        "C:<Remitente>\n"        -> CHAT  (💬) agrupado por remitente (xN)
//        "R:<Min>:<Reunión>\n"    -> REUNIÓN (⏰) con cuenta regresiva viva
//        "X\n"                    -> Limpia toda la cola
//        "B\n"                    -> Saludo remoto "Buen día!"
//    - Acercar la mano (<= 10 cm) con avisos activos muestra ✔ y limpia TODAS
//      las notificaciones pendientes de esa misma persona por nombre.
//
// 3) CONTROL DIRECTO DESDE ANTIGRAVITY (/robot) VÍA BLUETOOTH:
//        "W:<F|B|L|R|S|INV>[:ms]" -> Micro-paso seguro con freno anti-caídas
//        "N:<L|C|R|NO|SCAN|deg>"  -> Mover cuello + cara sincronizada
//        "G:<HAPPY|SURPRISE|HEART|THINK|CELEBRATE|SLEEP|WAKE>" -> Gestos/Sueño
//        "T:<Mensaje>"            -> Mostrar texto libre en matriz 8x8
//        "Q"                      -> Telemetría instantánea de sensores
// ============================================================================

#include <Wire.h>
#include <avr/pgmspace.h>

// --- Pines de Motores (forzados en LOW) ---
const int MR_CTRL    = 2;
const int ML_CTRL    = 4;
const int MR_PWM_ALT = 5;
const int ML_PWM     = 6;
const int MR_PWM     = 9;

// --- Servo del cuello (D10) ---
// Nota física: menor pulso (~960 µs) gira hacia x=0 (izquierda vista de frente);
//              mayor pulso (~2020 µs) gira hacia x=7 (derecha vista de frente).
const int SERVO_PIN          = 10;
const int PULSE_MIN_US       = 1070; // ~52° límite reposo
const int PULSE_CENTER_US    = 1490; // ~90° frente
const int PULSE_MAX_US       = 1910; // ~128° límite reposo
const int TRACK_PULSE_MIN_US = 960;  // ~40° límite seguimiento de mano
const int TRACK_PULSE_MAX_US = 2020; // ~140° límite seguimiento de mano
const int PULSE_STEP_US      = 7;    // Velocidad suave en reposo
const int TRACK_STEP_US      = 14;   // Velocidad ágil siguiendo tu mano
const int MIN_DELTA_US       = 260;  // Mínimo ~24° por giro aleatorio

// --- Tiempos ---
const unsigned long OBSERVE_MIN_MS = 5000UL;  // Pausa aleatoria mín: 5 s
const unsigned long OBSERVE_MAX_MS = 30000UL; // Pausa aleatoria máx: 30 s
const unsigned long ICON_HOLD_MS   = 1800UL;  // Segundos mostrando ícono antes del texto
const unsigned long NOTIF_REST_MS  = 16000UL; // Pausa mostrando carita viva entre recordatorios
const unsigned int  SCROLL_STEP_MS = 56;      // Velocidad ágil de desplazamiento de texto

// --- Sensor Ultrasónico ---
const int   TRIG_PIN        = 12;
const int   ECHO_PIN        = 13;
const float NEAR_DETECT_CM  = 10.0f; // Distancia para borrar avisos (<= 10 cm)
const float TRACK_START_CM  = 12.0f; // Distancia para enganchar tu mano en reposo
const float TRACK_HOLD_CM   = 26.0f; // Rango de seguimiento activo una vez enganchada

// --- Sensores Infrarrojos Inferiores (detector de levantamiento de la mesa) ---
const int LINE_L_PIN = 11;
const int LINE_M_PIN = 7;
const int LINE_R_PIN = 8;
bool deskCalibrated  = false;

// --- Matriz 8x8 HT16K33 (I2C 0x70) ---
const uint8_t HT16K33_ADDR            = 0x70;
const uint8_t BRIGHTNESS_NOTIF_ICON   = 15; // Brillo máximo para íconos de notificación
const uint8_t BRIGHTNESS_TEXT_GESTURE = 8;  // Brillo suave para letras y gestos/caritas

// --- Cola de Notificaciones ---
enum NotifType : uint8_t {
  NOTIF_NONE = 0,
  NOTIF_MAIL = 1,
  NOTIF_CHAT = 2,
  NOTIF_MEET = 3
};

enum NotifPhase : uint8_t {
  PHASE_ICON = 0,
  PHASE_TEXT = 1,
  PHASE_REST = 2
};

const uint8_t MAX_NOTIFS     = 10;
const uint8_t MAX_SENDER_LEN = 24;

struct NotifItem {
  NotifType type;
  uint8_t count;              // Cantidad de mensajes agrupados de este remitente
  int16_t startOffsetMin;     // Para NOTIF_MEET: minutos hasta iniciar al recibirlo
  unsigned long rxMillis;     // millis() al recibirlo para descontar en vivo
  char sender[MAX_SENDER_LEN];
};

NotifItem notifQueue[MAX_NOTIFS];
uint8_t notifCount      = 0;
uint8_t currentNotifIdx = 0;
NotifPhase notifPhase   = PHASE_ICON;

bool notifNeedsCenterPan       = false;
bool notifNeedsAttentionWiggle = false;
unsigned long phaseStartMs     = 0;
unsigned long lastNotifAnimMs  = 0;
int notifAnimStep              = 0;

// Buffer de recepción serie no bloqueante + cola de comando remoto /robot
char rxBuf[52];
uint8_t rxLen = 0;
bool triggerRemoteBuenDia = false;

enum RemoteCmdKind : uint8_t {
  RCMD_NONE    = 0,
  RCMD_WHEEL   = 1,
  RCMD_NECK    = 2,
  RCMD_GESTURE = 3,
  RCMD_TEXT    = 4
};

RemoteCmdKind pendingRemoteKind = RCMD_NONE;
char pendingRemotePayload[36]   = "";
bool isSleeping                 = false;
bool motorFlipForward           = false;

int currentPulseUs               = PULSE_CENTER_US;
unsigned long pauseStartMs       = 0;
unsigned long nextObservePauseMs = 15000UL;

// --- Estado de Cara Viva Permanente Sincronizada al Ángulo del Ultrasonido ---
uint8_t currentMatrixBrightness  = 255;
bool faceRenderDirty             = true;
int8_t currentGaze               = 0;   // -3 (izq extremo) .. 0 (centro) .. +3 (der extremo)
int8_t lastRenderedGaze          = 99;
uint8_t currentLid               = 3;   // 0=abierto, 1=bajando1, 2=bajando2, 3=cerrado
uint8_t lastRenderedLid          = 99;
uint8_t blinkSeqStep             = 0;   // 0=inactivo, 1..5=etapas del parpadeo suave
bool pendingDoubleBlink          = false;
unsigned long blinkStepStartMs   = 0;
unsigned long nextBlinkAtMs      = 0;

unsigned long pickRandomObservePauseMs() {
  return (unsigned long)random(OBSERVE_MIN_MS, OBSERVE_MAX_MS + 1UL);
}

unsigned long pickRandomBlinkDelayMs() {
  return (unsigned long)random(2600UL, 5600UL);
}

// ============================================================================
// ÍCONOS 8x8 (fila 0 = arriba .. fila 7 = abajo; bit 0 = izq .. bit 7 = der)
// ============================================================================

const uint8_t ICON_EMAIL[8] = {
  0x00, 0xFF, 0xC3, 0xA5, 0x99, 0x81, 0xFF, 0x00
};

const uint8_t ICON_CHAT_0[8] = {
  0x7E, 0x81, 0x81, 0x81, 0x81, 0x72, 0x0A, 0x06
};

const uint8_t ICON_CHAT_1[8] = {
  0x7E, 0x81, 0xBD, 0x81, 0x81, 0x72, 0x0A, 0x06
};

const uint8_t ICON_CHAT_2[8] = {
  0x7E, 0x81, 0xBD, 0x81, 0x9D, 0x72, 0x0A, 0x06
};

// Reloj despertador 8x8 (⏰) con aguja girando en sentido horario (12 -> 3 -> 6 -> 9)
const uint8_t ICON_MEET_0[8] = {
  0x41, 0x3E, 0x49, 0x49, 0x49, 0x41, 0x41, 0x3E
};

const uint8_t ICON_MEET_1[8] = {
  0x41, 0x3E, 0x41, 0x41, 0x59, 0x41, 0x41, 0x3E
};

const uint8_t ICON_MEET_2[8] = {
  0x41, 0x3E, 0x41, 0x41, 0x49, 0x49, 0x49, 0x3E
};

const uint8_t ICON_MEET_3[8] = {
  0x41, 0x3E, 0x41, 0x41, 0x4D, 0x41, 0x41, 0x3E
};

const uint8_t ICON_CHECK[8] = {
  0x00, 0x80, 0xC0, 0x60, 0x31, 0x1B, 0x0E, 0x04
};

// Carita sorprendida/desconcertada "O_o" y "o_O" cuando lo levantan del escritorio
const uint8_t ICON_LIFT_Oo[8] = {
  0x00, 0x06, 0x09, 0x69, 0x66, 0x00, 0x3C, 0x00
};

const uint8_t ICON_LIFT_oO[8] = {
  0x00, 0x60, 0x90, 0x96, 0x66, 0x00, 0x3C, 0x00
};

// Carita feliz "^_^" de alivio / alegría
const uint8_t ICON_HAPPY[8] = {
  0x00, 0x42, 0xA5, 0x00, 0x00, 0x42, 0x3C, 0x00
};

// Íconos adicionales en PROGMEM para gestos /robot (0 bytes de RAM)
const uint8_t ICON_HEART_BIG[8] PROGMEM = {
  0x00, 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18
};

const uint8_t ICON_HEART_SMALL[8] PROGMEM = {
  0x00, 0x00, 0x24, 0x7E, 0x7E, 0x3C, 0x18, 0x00
};

const uint8_t ICON_THINK_0[8] PROGMEM = {
  0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00
};

const uint8_t ICON_THINK_1[8] PROGMEM = {
  0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00
};

const uint8_t ICON_THINK_2[8] PROGMEM = {
  0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00
};

const uint8_t ICON_SLEEP_0[8] PROGMEM = {
  0x00, 0x00, 0x00, 0x42, 0x24, 0x00, 0x18, 0x00
};

const uint8_t ICON_SLEEP_1[8] PROGMEM = {
  0x80, 0x40, 0x00, 0x42, 0x24, 0x00, 0x18, 0x00
};

// ============================================================================
// FUENTE PROPORCIONAL 8 FILAS (bit 0 = arriba y=0, bit 7 = abajo y=7)
//   0x80=á  0x81=é  0x82=í  0x83=ó  0x84=ú  0x85=ñ  0x86=Ñ  0x87=ü
// ============================================================================
struct GlyphDef {
  uint8_t code;
  uint8_t width;
  uint8_t cols[5];
};

const GlyphDef FONT_TABLE[] PROGMEM = {
  // Números
  {'0', 4, {0x7C, 0xA2, 0x92, 0x7C, 0}},
  {'1', 3, {0x84, 0xFE, 0x80, 0, 0}},
  {'2', 4, {0xC4, 0xA2, 0x92, 0x8C, 0}},
  {'3', 4, {0x44, 0x82, 0x92, 0x6C, 0}},
  {'4', 4, {0x30, 0x28, 0x24, 0xFE, 0}},
  {'5', 4, {0x4E, 0x8A, 0x8A, 0x72, 0}},
  {'6', 4, {0x7C, 0x92, 0x92, 0x64, 0}},
  {'7', 4, {0x02, 0xE2, 0x12, 0x0E, 0}},
  {'8', 4, {0x6C, 0x92, 0x92, 0x6C, 0}},
  {'9', 4, {0x4C, 0x92, 0x92, 0x7C, 0}},

  // Mayúsculas
  {'A', 4, {0xFC, 0x12, 0x12, 0xFC, 0}},
  {'B', 5, {0xFE, 0x92, 0x92, 0x92, 0x6C}},
  {'C', 4, {0x7C, 0x82, 0x82, 0x44, 0}},
  {'D', 4, {0xFE, 0x82, 0x82, 0x7C, 0}},
  {'E', 4, {0xFE, 0x92, 0x92, 0x82, 0}},
  {'F', 4, {0xFE, 0x12, 0x12, 0x02, 0}},
  {'G', 4, {0x7C, 0x82, 0x92, 0x74, 0}},
  {'H', 4, {0xFE, 0x10, 0x10, 0xFE, 0}},
  {'I', 3, {0x82, 0xFE, 0x82, 0, 0}},
  {'J', 4, {0x40, 0x80, 0x82, 0x7E, 0}},
  {'K', 4, {0xFE, 0x10, 0x28, 0xC6, 0}},
  {'L', 4, {0xFE, 0x80, 0x80, 0x80, 0}},
  {'M', 5, {0xFE, 0x04, 0x08, 0x04, 0xFE}},
  {'N', 5, {0xFE, 0x08, 0x10, 0x20, 0xFE}},
  {'O', 4, {0x7C, 0x82, 0x82, 0x7C, 0}},
  {'P', 4, {0xFE, 0x12, 0x12, 0x0C, 0}},
  {'Q', 5, {0x7C, 0x82, 0xA2, 0x42, 0xBC}},
  {'R', 4, {0xFE, 0x12, 0x32, 0xCC, 0}},
  {'S', 4, {0x4C, 0x92, 0x92, 0x64, 0}},
  {'T', 3, {0x02, 0xFE, 0x02, 0, 0}},
  {'U', 4, {0x7E, 0x80, 0x80, 0x7E, 0}},
  {'V', 5, {0x3E, 0x40, 0x80, 0x40, 0x3E}},
  {'W', 5, {0x7E, 0x80, 0x70, 0x80, 0x7E}},
  {'X', 5, {0xC6, 0x28, 0x10, 0x28, 0xC6}},
  {'Y', 5, {0x0E, 0x10, 0xE0, 0x10, 0x0E}},
  {'Z', 4, {0xC2, 0xA2, 0x92, 0x8E, 0}},

  // Minúsculas
  {'a', 4, {0x40, 0xA8, 0xA8, 0xF0, 0}},
  {'b', 4, {0xFE, 0x88, 0x88, 0x70, 0}},
  {'c', 4, {0x70, 0x88, 0x88, 0x50, 0}},
  {'d', 4, {0x70, 0x88, 0x88, 0xFE, 0}},
  {'e', 4, {0x70, 0xA8, 0xA8, 0xB0, 0}},
  {'f', 3, {0x10, 0xFC, 0x12, 0, 0}},
  {'g', 4, {0x30, 0xA8, 0xA8, 0x78, 0}},
  {'h', 4, {0xFE, 0x08, 0x08, 0xF0, 0}},
  {'i', 3, {0x88, 0xFA, 0x80, 0, 0}},
  {'j', 3, {0x40, 0x80, 0xFA, 0, 0}},
  {'k', 4, {0xFE, 0x20, 0x50, 0x88, 0}},
  {'l', 3, {0x82, 0xFE, 0x80, 0, 0}},
  {'m', 5, {0xF8, 0x08, 0xF0, 0x08, 0xF0}},
  {'n', 4, {0xF8, 0x08, 0x08, 0xF0, 0}},
  {'o', 4, {0x70, 0x88, 0x88, 0x70, 0}},
  {'p', 4, {0xF8, 0x28, 0x28, 0x10, 0}},
  {'q', 4, {0x10, 0x28, 0x28, 0xF8, 0}},
  {'r', 3, {0xF8, 0x10, 0x08, 0, 0}},
  {'s', 4, {0x90, 0xA8, 0xA8, 0x48, 0}},
  {'t', 3, {0x08, 0x7E, 0x88, 0, 0}},
  {'u', 4, {0x78, 0x80, 0x80, 0xF8, 0}},
  {'v', 4, {0x78, 0x80, 0x40, 0x38, 0}},
  {'w', 5, {0x78, 0x80, 0x60, 0x80, 0x78}},
  {'x', 4, {0x88, 0x50, 0x50, 0x88, 0}},
  {'y', 4, {0x98, 0xA0, 0xA0, 0x78, 0}},
  {'z', 4, {0x88, 0xC8, 0xA8, 0x98, 0}},

  // Acentos y eñe en español
  {0x80, 4, {0x40, 0xA8, 0xAA, 0xF1, 0}},    // á
  {0x81, 4, {0x70, 0xA8, 0xAA, 0xB1, 0}},    // é
  {0x82, 3, {0x88, 0xFA, 0x81, 0, 0}},       // í
  {0x83, 4, {0x70, 0x88, 0x8A, 0x71, 0}},    // ó
  {0x84, 4, {0x78, 0x80, 0x82, 0xF9, 0}},    // ú
  {0x85, 4, {0xFA, 0x09, 0x0A, 0xF1, 0}},    // ñ
  {0x86, 5, {0xFC, 0x09, 0x11, 0x21, 0xFC}}, // Ñ
  {0x87, 4, {0x79, 0x80, 0x80, 0xF9, 0}},    // ü

  // Signos comunes
  {'!', 1, {0xBE, 0, 0, 0, 0}},
  {'.', 1, {0x80, 0, 0, 0, 0}},
  {',', 2, {0x80, 0x40, 0, 0, 0}},
  {':', 1, {0x48, 0, 0, 0, 0}},
  {'-', 3, {0x10, 0x10, 0x10, 0, 0}},
  {'_', 3, {0x80, 0x80, 0x80, 0, 0}},
  {'/', 4, {0x80, 0x60, 0x18, 0x06, 0}},
  {'(', 2, {0x7C, 0x82, 0, 0, 0}},
  {')', 2, {0x82, 0x7C, 0, 0, 0}},
  {'?', 4, {0x04, 0x02, 0xB2, 0x0C, 0}},
  {'@', 5, {0x7C, 0x82, 0xBA, 0xAA, 0x3C}},
  {'+', 3, {0x10, 0x38, 0x10, 0, 0}}
};

const uint8_t FONT_COUNT = sizeof(FONT_TABLE) / sizeof(FONT_TABLE[0]);

const char MSG_BUEN_DIA[] = "Buen d\x82" "a!";

uint8_t getGlyph(uint8_t ch, uint8_t outCols[5]) {
  if (ch == ' ') {
    outCols[0] = 0x00;
    outCols[1] = 0x00;
    return 2;
  }
  for (uint8_t i = 0; i < FONT_COUNT; i++) {
    uint8_t code = pgm_read_byte(&FONT_TABLE[i].code);
    if (code == ch) {
      uint8_t w = pgm_read_byte(&FONT_TABLE[i].width);
      for (uint8_t c = 0; c < w; c++) {
        outCols[c] = pgm_read_byte(&FONT_TABLE[i].cols[c]);
      }
      return w;
    }
  }
  outCols[0] = 0x10;
  outCols[1] = 0x10;
  return 2;
}

// --- Funciones de Matriz HT16K33 ---
void setMatrixBrightness(uint8_t level) {
  if (level > 15) level = 15;
  if (currentMatrixBrightness == level) return;
  currentMatrixBrightness = level;
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0xE0 | level);
  Wire.endTransmission();
}

void ht16k33Init() {
  Wire.begin();
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x21);
  Wire.endTransmission();

  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x81);
  Wire.endTransmission();

  currentMatrixBrightness = 255;
  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
}

void writeMatrixRowsRaw(const uint8_t rowsTopToBottom[8]) {
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x00);
  for (int r = 0; r < 8; r++) {
    Wire.write(rowsTopToBottom[7 - r]);
    Wire.write(0x00);
  }
  Wire.endTransmission();
}

void showRows(const uint8_t rowsTopToBottom[8]) {
  faceRenderDirty = true;
  writeMatrixRowsRaw(rowsTopToBottom);
}

void showRowsP(const uint8_t iconProgmem[8]) {
  uint8_t buf[8];
  for (uint8_t i = 0; i < 8; i++) {
    buf[i] = pgm_read_byte(&iconProgmem[i]);
  }
  showRows(buf);
}

void clearScreen() {
  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  const uint8_t empty[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  showRows(empty);
}

// --- Motor de Cara Viva Sincronizada 1:1 con el Ángulo del Ultrasonido ---
// Convierte el pulso real del cuello (currentPulseUs) en 7 zonas continuas (-3..+3)
// con histéresis de ±8 µs para evitar parpadeo en bordes:
//   -3: extremo izq  (< 1170 µs)
//   -2: izquierda    (1170..1299 µs)
//   -1: semi-izq     (1300..1424 µs)
//    0: centro       (1425..1555 µs, ~90° frente exacto)
//   +1: semi-der     (1556..1680 µs)
//   +2: derecha      (1681..1810 µs)
//   +3: extremo der  (> 1810 µs)
int8_t gazeZoneFromPulseUs(int pulseUs, int8_t prevZone) {
  const int bounds[6] = {1170, 1300, 1425, 1555, 1680, 1810};
  int8_t rawZone = 3;
  for (uint8_t i = 0; i < 6; i++) {
    if (pulseUs < bounds[i]) {
      rawZone = (int8_t)i - 3;
      break;
    }
  }
  if (prevZone >= -3 && prevZone <= 3 && abs((int)rawZone - (int)prevZone) == 1) {
    uint8_t boundaryIdx = (uint8_t)((rawZone > prevZone) ? (prevZone + 3) : (rawZone + 3));
    if (abs(pulseUs - bounds[boundaryIdx]) < 8) {
      return prevZone;
    }
  }
  return rawZone;
}

void drawLivingFacePixels(int8_t gaze, uint8_t lid) {
  if (gaze < -3) gaze = -3;
  if (gaze >  3) gaze =  3;
  if (lid  >  3) lid  =  3;

  uint8_t eyeCols    = 0x66;
  uint8_t topRowMask = 0x66;
  uint8_t mouth      = 0x18;

  if (gaze == -3) {
    eyeCols    = 0x1B; // x=0,1 y x=3,4 (extremo izquierdo con perspectiva)
    topRowMask = 0x03;
    mouth      = 0x06;
  } else if (gaze == -2) {
    eyeCols    = 0x33; // x=0,1 y x=4,5 (izquierda con perspectiva)
    topRowMask = 0x03;
    mouth      = 0x0C;
  } else if (gaze == -1) {
    eyeCols    = 0x33; // x=0,1 y x=4,5 (semi-izquierda)
    topRowMask = 0x33;
    mouth      = 0x0C;
  } else if (gaze == 1) {
    eyeCols    = 0xCC; // x=2,3 y x=6,7 (semi-derecha)
    topRowMask = 0xCC;
    mouth      = 0x30;
  } else if (gaze == 2) {
    eyeCols    = 0xCC; // x=2,3 y x=6,7 (derecha con perspectiva)
    topRowMask = 0xC0;
    mouth      = 0x30;
  } else if (gaze == 3) {
    eyeCols    = 0xD8; // x=3,4 y x=6,7 (extremo derecho con perspectiva)
    topRowMask = 0xC0;
    mouth      = 0x60;
  }

  uint8_t rows[8];
  rows[0] = 0x00;
  rows[1] = (lid == 0) ? topRowMask : 0x00;
  rows[2] = (lid <= 1) ? eyeCols    : 0x00;
  rows[3] = eyeCols; // Línea de pestañas cuando el ojo se cierra en lid == 3
  rows[4] = (lid <= 2) ? eyeCols    : 0x00;
  rows[5] = 0x00;
  rows[6] = mouth;
  rows[7] = 0x00;

  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  writeMatrixRowsRaw(rows);
  lastRenderedGaze = gaze;
  lastRenderedLid  = lid;
  faceRenderDirty  = false;
}

void resumeLivingFaceSmooth(int8_t unused = 0) {
  (void)unused;
  unsigned long now  = millis();
  currentGaze        = gazeZoneFromPulseUs(currentPulseUs, currentGaze);
  pendingDoubleBlink = false;
  // Reabrir suavemente los párpados (3 -> 2 -> 1 -> 0) exactamente en el ángulo actual
  currentLid         = 3;
  blinkSeqStep       = 3;
  blinkStepStartMs   = now;
  nextBlinkAtMs      = now + pickRandomBlinkDelayMs();
  faceRenderDirty    = true;
  drawLivingFacePixels(currentGaze, currentLid);
}

void updateLivingFace() {
  unsigned long now = millis();

  // Sincronización directa en tiempo real con el ángulo físico del ultrasonido
  currentGaze = gazeZoneFromPulseUs(currentPulseUs, currentGaze);

  // Cortina suave del párpado superior (0 -> 1 -> 2 -> 3 -> 2 -> 1 -> 0)
  if (blinkSeqStep == 0) {
    if (now >= nextBlinkAtMs) {
      blinkSeqStep     = 1;
      currentLid       = 1;
      blinkStepStartMs = now;
      if (!pendingDoubleBlink) {
        pendingDoubleBlink = (random(0, 100) < 20);
      }
    }
  } else if (blinkSeqStep == 1 && (now - blinkStepStartMs >= 28UL)) {
    blinkSeqStep     = 2;
    currentLid       = 2;
    blinkStepStartMs = now;
  } else if (blinkSeqStep == 2 && (now - blinkStepStartMs >= 28UL)) {
    blinkSeqStep     = 3;
    currentLid       = 3;
    blinkStepStartMs = now;
  } else if (blinkSeqStep == 3 && (now - blinkStepStartMs >= 52UL)) {
    blinkSeqStep     = 4;
    currentLid       = 2;
    blinkStepStartMs = now;
  } else if (blinkSeqStep == 4 && (now - blinkStepStartMs >= 32UL)) {
    blinkSeqStep     = 5;
    currentLid       = 1;
    blinkStepStartMs = now;
  } else if (blinkSeqStep == 5 && (now - blinkStepStartMs >= 32UL)) {
    blinkSeqStep = 0;
    currentLid   = 0;
    if (pendingDoubleBlink) {
      pendingDoubleBlink = false;
      nextBlinkAtMs      = now + 145UL;
    } else {
      nextBlinkAtMs      = now + pickRandomBlinkDelayMs();
    }
  }

  if (faceRenderDirty || currentGaze != lastRenderedGaze || currentLid != lastRenderedLid) {
    drawLivingFacePixels(currentGaze, currentLid);
  }
}

// --- Lectura Ultrasónica rápida (proximidad <= 10 cm) ---
float readDistanceFastCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 4500UL);
  if (duration == 0) {
    return 999.0f;
  }
  return (duration * 0.0343f) / 2.0f;
}

bool isSomeoneNear() {
  float d1 = readDistanceFastCm();
  if (d1 > NEAR_DETECT_CM) {
    return false;
  }
  delay(8);
  float d2 = readDistanceFastCm();
  return (d2 <= NEAR_DETECT_CM + 1.5f);
}

bool isHandReadyToTrack() {
  float d1 = readDistanceFastCm();
  if (d1 > TRACK_START_CM) {
    return false;
  }
  delay(8);
  float d2 = readDistanceFastCm();
  return (d2 <= TRACK_START_CM + 2.0f);
}

// Lectura rápida para bucle de seguimiento 50 Hz (timeout corto ~44 cm)
float readTrackDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 2600UL);
  if (duration == 0) {
    // Segundo pulso rápido por si la mano estaba inclinada
    delayMicroseconds(200);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    duration = pulseIn(ECHO_PIN, HIGH, 2600UL);
    if (duration == 0) {
      return 999.0f;
    }
  }
  return (duration * 0.0343f) / 2.0f;
}

// --- Detección de levantamiento mediante los 3 sensores IR inferiores ---
bool rawSensorsOnDesk() {
  // Sobre la mesa al menos uno refleja luz IR (LOW = 0); en el aire los 3 dan HIGH (1/1/1)
  return (digitalRead(LINE_L_PIN) == LOW ||
          digitalRead(LINE_M_PIN) == LOW ||
          digitalRead(LINE_R_PIN) == LOW);
}

bool isRobotLifted() {
  if (rawSensorsOnDesk()) {
    deskCalibrated = true;
    return false;
  }
  if (!deskCalibrated) {
    return false;
  }
  delay(12);
  return !rawSensorsOnDesk();
}

void pulseServoOnce(int pulseUs) {
  digitalWrite(SERVO_PIN, HIGH);
  delayMicroseconds(pulseUs);
  digitalWrite(SERVO_PIN, LOW);
}

void handleLiftedReaction() {
  // Mostrar de inmediato "O_o" en brillo 8 y mirar desconcertado a ambos lados
  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  showRows(ICON_LIFT_Oo);

  const int lookLeft  = PULSE_CENTER_US + 180;
  const int lookRight = PULSE_CENTER_US - 180;

  // Pequeña sacudida curiosa de cabeza si sigue en el aire
  int targets[3] = {lookRight, lookLeft, PULSE_CENTER_US};
  for (uint8_t t = 0; t < 3 && !rawSensorsOnDesk(); t++) {
    while (currentPulseUs != targets[t] && !rawSensorsOnDesk()) {
      unsigned long f0 = micros();
      pollBluetooth();
      int diff = targets[t] - currentPulseUs;
      if (abs(diff) <= 10) currentPulseUs = targets[t];
      else currentPulseUs += (diff > 0) ? 10 : -10;
      pulseServoOnce(currentPulseUs);
      if (((millis() / 420UL) % 2UL) == 0UL) showRows(ICON_LIFT_Oo);
      else showRows(ICON_LIFT_oO);
      unsigned long el = micros() - f0;
      if (el < 20000UL) {
        delay((20000UL - el) / 1000UL);
        delayMicroseconds((20000UL - el) % 1000UL);
      }
    }
  }
  digitalWrite(SERVO_PIN, LOW);

  // Mantener alternando "O_o" <-> "o_O" todo el tiempo que permanezca levantado
  while (!rawSensorsOnDesk()) {
    pollBluetooth();
    if (((millis() / 450UL) % 2UL) == 0UL) {
      showRows(ICON_LIFT_Oo);
    } else {
      showRows(ICON_LIFT_oO);
    }
    delay(40);
  }

  // Al apoyarlo de vuelta en la mesa: carita feliz "^_^" de alivio
  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  showRows(ICON_HAPPY);
  unsigned long tBack = millis();
  while (millis() - tBack < 900UL) {
    pollBluetooth();
    delay(30);
  }

  if (notifCount == 0) {
    resumeLivingFaceSmooth(0);
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
  } else if (notifPhase == PHASE_REST) {
    resumeLivingFaceSmooth(0);
    phaseStartMs = millis();
  } else {
    notifPhase      = PHASE_ICON;
    phaseStartMs    = millis();
    lastNotifAnimMs = 0;
  }
}

long currentMeetingRemainingMin(const NotifItem& item) {
  long elapsedMin = (long)((millis() - item.rxMillis) / 60000UL);
  return (long)item.startOffsetMin - elapsedMin;
}

// Comparación insensible a mayúsculas ASCII (conservando acentos 0x80..0x87)
bool sameSenderName(const char* a, const char* b) {
  while (*a != '\0' && *b != '\0') {
    uint8_t ca = (uint8_t)(*a);
    uint8_t cb = (uint8_t)(*b);
    if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
    if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
    if (ca != cb) return false;
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

void beginNotifPhaseIcon(uint8_t idx, bool recenter) {
  currentNotifIdx = idx;
  notifPhase      = PHASE_ICON;
  phaseStartMs    = millis();
  lastNotifAnimMs = 0;
  notifAnimStep   = 0;
  if (recenter) {
    notifNeedsCenterPan = true;
  }
}

// --- Gestión de Cola de Notificaciones (con agrupación por remitente) ---
void enqueueNotification(NotifType type, const char* senderRaw, int16_t meetOffsetMin = 0) {
  isSleeping = false;

  while (*senderRaw == ' ' || *senderRaw == ':') {
    senderRaw++;
  }

  char cleanName[MAX_SENDER_LEN];
  if (*senderRaw == '\0') {
    if (type == NOTIF_MAIL) {
      strncpy(cleanName, "Correo nuevo", MAX_SENDER_LEN - 1);
    } else if (type == NOTIF_MEET) {
      strncpy(cleanName, "Reuni\x83n", MAX_SENDER_LEN - 1);
    } else {
      strncpy(cleanName, "Mensaje nuevo", MAX_SENDER_LEN - 1);
    }
  } else {
    strncpy(cleanName, senderRaw, MAX_SENDER_LEN - 1);
  }
  cleanName[MAX_SENDER_LEN - 1] = '\0';

  // Si es una reunión que ya estaba en la cola, actualizar su tiempo en vez de duplicarla
  if (type == NOTIF_MEET) {
    for (uint8_t i = 0; i < notifCount; i++) {
      if (notifQueue[i].type == NOTIF_MEET && sameSenderName(notifQueue[i].sender, cleanName)) {
        notifQueue[i].startOffsetMin = meetOffsetMin;
        notifQueue[i].rxMillis       = millis();
        if (meetOffsetMin <= 0) {
          beginNotifPhaseIcon(i, false);
          notifNeedsAttentionWiggle = true;
        }
        return;
      }
    }
  } else {
    // Si la misma persona envía varios mensajes del mismo tipo, agruparlos en uno solo (xN)
    for (uint8_t i = 0; i < notifCount; i++) {
      if (notifQueue[i].type == type && sameSenderName(notifQueue[i].sender, cleanName)) {
        if (notifQueue[i].count < 99) {
          notifQueue[i].count++;
        }
        if (notifPhase == PHASE_REST) {
          beginNotifPhaseIcon(i, true);
        }
        return;
      }
    }
  }

  if (notifCount >= MAX_NOTIFS) {
    for (uint8_t i = 0; i < MAX_NOTIFS - 1; i++) {
      notifQueue[i] = notifQueue[i + 1];
    }
    notifCount = MAX_NOTIFS - 1;
    if (currentNotifIdx > 0) {
      currentNotifIdx--;
    }
  }

  NotifItem& item = notifQueue[notifCount];
  item.type           = type;
  item.count          = 1;
  item.startOffsetMin = meetOffsetMin;
  item.rxMillis       = millis();
  strncpy(item.sender, cleanName, MAX_SENDER_LEN);
  item.sender[MAX_SENDER_LEN - 1] = '\0';

  notifCount++;

  if (type == NOTIF_MEET) {
    notifNeedsAttentionWiggle = true;
  }

  // Mostrar de inmediato cada aviso recién llegado antes de volver a la carita viva
  beginNotifPhaseIcon(notifCount - 1, true);
}

void parseMeetingPayload(const char* payload) {
  // Espera "<min>:<titulo>" (ej. "10:Coordinacion" o "-2:Daily")
  while (*payload == ' ' || *payload == ':') {
    payload++;
  }
  const char* colon = strchr(payload, ':');
  if (colon != NULL) {
    char numBuf[10];
    size_t nlen = (size_t)(colon - payload);
    if (nlen >= sizeof(numBuf)) nlen = sizeof(numBuf) - 1;
    memcpy(numBuf, payload, nlen);
    numBuf[nlen] = '\0';
    int16_t mins = (int16_t)atoi(numBuf);
    enqueueNotification(NOTIF_MEET, colon + 1, mins);
  } else {
    enqueueNotification(NOTIF_MEET, payload, 5);
  }
}

void printRobotStatus() {
  float d = readDistanceFastCm();
  Serial.print(F("STAT:DIST="));
  if (d >= 900.0f) {
    Serial.print(F("LIBRE"));
  } else {
    Serial.print((int)(d + 0.5f));
  }
  Serial.print(F(",DESK="));
  Serial.print(rawSensorsOnDesk() ? 1 : 0);
  Serial.print(F(",PULSE="));
  Serial.print(currentPulseUs);
  Serial.print(F(",ZONE="));
  Serial.print((int)gazeZoneFromPulseUs(currentPulseUs, currentGaze));
  Serial.print(F(",NOTIF="));
  Serial.print((int)notifCount);
  Serial.print(F(",SLEEP="));
  Serial.println(isSleeping ? 1 : 0);
}

void queueRemoteCmd(RemoteCmdKind kind, const char* payload) {
  while (*payload == ' ' || *payload == ':') {
    payload++;
  }
  strncpy(pendingRemotePayload, payload, sizeof(pendingRemotePayload) - 1);
  pendingRemotePayload[sizeof(pendingRemotePayload) - 1] = '\0';
  pendingRemoteKind = kind;
}

void processBluetoothLine(char* line) {
  int len = strlen(line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
    line[--len] = '\0';
  }
  if (len == 0) return;

  char cmd = line[0];
  const char* payload = (len > 1 && line[1] == ':') ? (line + 2) : (line + 1);

  if (cmd == 'M' || cmd == 'm') {
    enqueueNotification(NOTIF_MAIL, payload);
    Serial.println(F("OK:MAIL"));
  } else if (cmd == 'C' || cmd == 'c' || cmd == 'H' || cmd == 'h') {
    enqueueNotification(NOTIF_CHAT, payload);
    Serial.println(F("OK:CHAT"));
  } else if (cmd == 'R' || cmd == 'r' || cmd == 'K' || cmd == 'k') {
    parseMeetingPayload(payload);
    Serial.println(F("OK:MEET"));
  } else if (cmd == 'X' || cmd == 'x') {
    notifCount = 0;
    currentNotifIdx = 0;
    resumeLivingFaceSmooth(0);
    Serial.println(F("OK:CLR"));
  } else if (cmd == 'B' || cmd == 'b') {
    triggerRemoteBuenDia = true;
    Serial.println(F("OK:BUENDIA"));
  } else if (cmd == 'W' || cmd == 'w') {
    queueRemoteCmd(RCMD_WHEEL, payload);
  } else if (cmd == 'N' || cmd == 'n') {
    queueRemoteCmd(RCMD_NECK, payload);
  } else if (cmd == 'G' || cmd == 'g') {
    queueRemoteCmd(RCMD_GESTURE, payload);
  } else if (cmd == 'T' || cmd == 't') {
    queueRemoteCmd(RCMD_TEXT, payload);
  } else if (cmd == 'Q' || cmd == 'q') {
    printRobotStatus();
  }
}

void pollBluetooth() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLen > 0) {
        rxBuf[rxLen] = '\0';
        processBluetoothLine(rxBuf);
        rxLen = 0;
      }
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    }
  }
}

// --- Desplazamiento de texto por la matriz 8x8 ---
bool scrollText(const char* text, bool abortOnNear, bool abortOnNewNotifWhenIdle) {
  uint8_t cols[250];
  int totalCols = 0;

  for (int i = 0; text[i] != '\0' && totalCols < (int)sizeof(cols) - 6; i++) {
    uint8_t gCols[5];
    uint8_t w = getGlyph((uint8_t)text[i], gCols);
    for (uint8_t c = 0; c < w && totalCols < (int)sizeof(cols); c++) {
      cols[totalCols++] = gCols[c];
    }
    if (text[i] != ' ' && text[i + 1] != '\0' && text[i + 1] != ' ' && totalCols < (int)sizeof(cols)) {
      cols[totalCols++] = 0x00;
    }
  }

  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);

  for (int offset = -7; offset <= totalCols; offset++) {
    pollBluetooth();

    if (isRobotLifted()) {
      handleLiftedReaction();
      return false;
    }
    if (pendingRemoteKind != RCMD_NONE) {
      return false;
    }
    if (abortOnNewNotifWhenIdle && notifCount > 0) {
      return false;
    }
    if (abortOnNear && isSomeoneNear()) {
      return true;
    }

    uint8_t rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int x = 0; x < 8; x++) {
      int colIdx = offset + x;
      if (colIdx >= 0 && colIdx < totalCols) {
        uint8_t colBits = cols[colIdx];
        for (int y = 0; y < 8; y++) {
          if (colBits & (1 << y)) {
            rows[y] |= (1 << x);
          }
        }
      }
    }

    showRows(rows);
    delay(SCROLL_STEP_MS);
  }

  return false;
}

void dismissCurrentNotification() {
  if (notifCount == 0) return;

  // Limpiar TODAS las notificaciones que pertenezcan a esta misma persona por nombre
  char targetName[MAX_SENDER_LEN];
  strncpy(targetName, notifQueue[currentNotifIdx].sender, MAX_SENDER_LEN - 1);
  targetName[MAX_SENDER_LEN - 1] = '\0';

  uint8_t writeIdx = 0;
  for (uint8_t readIdx = 0; readIdx < notifCount; readIdx++) {
    if (!sameSenderName(notifQueue[readIdx].sender, targetName)) {
      if (writeIdx != readIdx) {
        notifQueue[writeIdx] = notifQueue[readIdx];
      }
      writeIdx++;
    }
  }
  notifCount = writeIdx;

  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  showRows(ICON_CHECK);

  unsigned long t0 = millis();
  while (millis() - t0 < 650UL) {
    pollBluetooth();
    delay(25);
  }

  uint8_t clearReads = 0;
  unsigned long waitReleaseStart = millis();
  while (clearReads < 3 && (millis() - waitReleaseStart < 2500UL)) {
    pollBluetooth();
    if (readDistanceFastCm() > 14.0f) {
      clearReads++;
    } else {
      clearReads = 0;
    }
    delay(40);
  }
  delay(250);

  if (notifCount > 0) {
    beginNotifPhaseIcon((currentNotifIdx >= notifCount) ? 0 : currentNotifIdx, true);
  } else {
    currentNotifIdx    = 0;
    resumeLivingFaceSmooth(0);
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
  }
}

int pickRandomTargetUs() {
  int candidate = currentPulseUs;
  for (int tries = 0; tries < 12; tries++) {
    candidate = (int)random(PULSE_MIN_US, PULSE_MAX_US + 1);
    if (abs(candidate - currentPulseUs) >= MIN_DELTA_US) {
      break;
    }
  }
  return candidate;
}

int computeEasedStep(int traveledUs, int remainingUs, int minStep, int maxStep) {
  int rampIn  = minStep + (traveledUs / 32);
  int rampOut = minStep + (remainingUs / 26);
  int step    = (rampIn < rampOut) ? rampIn : rampOut;
  if (step > maxStep) step = maxStep;
  if (step < minStep) step = minStep;
  return step;
}

bool panSmoothTo(int targetPulseUs, bool stopOnNear = true, bool animateFace = false) {
  int nearHits     = 0;
  int startPulseUs = currentPulseUs;

  while (currentPulseUs != targetPulseUs) {
    unsigned long frameStartUs = micros();

    pollBluetooth();
    if (stopOnNear && (notifCount > 0 || pendingRemoteKind != RCMD_NONE || !rawSensorsOnDesk())) {
      digitalWrite(SERVO_PIN, LOW);
      return false;
    }

    int diff    = targetPulseUs - currentPulseUs;
    int absDiff = abs(diff);
    int stepUs  = animateFace
                  ? computeEasedStep(abs(currentPulseUs - startPulseUs), absDiff, 3, 8)
                  : computeEasedStep(abs(currentPulseUs - startPulseUs), absDiff, 4, 9);

    if (absDiff <= stepUs) {
      currentPulseUs = targetPulseUs;
    } else if (diff > 0) {
      currentPulseUs += stepUs;
    } else {
      currentPulseUs -= stepUs;
    }

    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(currentPulseUs);
    digitalWrite(SERVO_PIN, LOW);

    if (animateFace) {
      updateLivingFace();
    }

    if (stopOnNear) {
      if (readDistanceFastCm() <= TRACK_START_CM) {
        nearHits++;
        if (nearHits >= 2) {
          digitalWrite(SERVO_PIN, LOW);
          return true;
        }
      } else {
        nearHits = 0;
      }
    }

    unsigned long elapsedUs = micros() - frameStartUs;
    if (elapsedUs < 20000UL) {
      unsigned long remUs = 20000UL - elapsedUs;
      if (remUs > 1000UL) {
        delay(remUs / 1000UL);
      }
      delayMicroseconds(remUs % 1000UL);
    }
  }

  digitalWrite(SERVO_PIN, LOW);

  if (animateFace) {
    updateLivingFace();
  }
  return false;
}

bool waitInteractiveMs(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    pollBluetooth();
    if (isRobotLifted()) {
      handleLiftedReaction();
      return false;
    }
    if (notifCount > 0) {
      return false;
    }
    delay(20);
  }
  return true;
}

// --- Seguimiento activo de tu mano con el cuello + cara sincronizada al ángulo ---
enum TrackState : uint8_t {
  TRK_LOCKED    = 0,
  TRK_PROBE_1   = 1,
  TRK_PROBE_2   = 2,
  TRK_CENTERING = 3
};

void trackHandWithEyes() {
  TrackState state     = TRK_LOCKED;
  int8_t trackDir      = 1;
  uint8_t centerFrames = 0;
  int curTrackStepUs   = 5;
  int sweepStartUs     = currentPulseUs;

  while (true) {
    unsigned long frameStartUs = micros();

    pollBluetooth();
    if (isRobotLifted()) {
      digitalWrite(SERVO_PIN, LOW);
      handleLiftedReaction();
      return;
    }
    if (notifCount > 0 || pendingRemoteKind != RCMD_NONE) {
      digitalWrite(SERVO_PIN, LOW);
      return;
    }

    float dist      = readTrackDistanceCm();
    bool handSeen   = (dist <= TRACK_HOLD_CM);
    bool movedServo = false;

    if (state == TRK_LOCKED) {
      curTrackStepUs = 5;
      if (!handSeen) {
        state          = TRK_PROBE_1;
        sweepStartUs   = currentPulseUs;
        curTrackStepUs = 5;
      }
    }

    if (state == TRK_PROBE_1) {
      if (handSeen) {
        state        = TRK_CENTERING;
        centerFrames = 4;
      } else {
        if (curTrackStepUs < TRACK_STEP_US) {
          curTrackStepUs += 2;
          if (curTrackStepUs > TRACK_STEP_US) curTrackStepUs = TRACK_STEP_US;
        }
        int nextUs       = currentPulseUs + (int)trackDir * curTrackStepUs;
        bool hitLimit    = (nextUs < TRACK_PULSE_MIN_US || nextUs > TRACK_PULSE_MAX_US);
        bool sweptEnough = (abs(nextUs - sweepStartUs) >= 240);

        if (hitLimit || sweptEnough) {
          trackDir       = -trackDir;
          curTrackStepUs = 5;
          state          = TRK_PROBE_2;
        } else {
          currentPulseUs = nextUs;
          movedServo     = true;
        }
      }
    } else if (state == TRK_PROBE_2) {
      if (handSeen) {
        state        = TRK_CENTERING;
        centerFrames = 4;
      } else {
        if (curTrackStepUs < TRACK_STEP_US) {
          curTrackStepUs += 2;
          if (curTrackStepUs > TRACK_STEP_US) curTrackStepUs = TRACK_STEP_US;
        }
        int nextUs          = currentPulseUs + (int)trackDir * curTrackStepUs;
        bool hitLimit       = (nextUs < TRACK_PULSE_MIN_US || nextUs > TRACK_PULSE_MAX_US);
        bool sweptOtherSide = (abs(nextUs - sweepStartUs) >= 340);

        if (hitLimit || sweptOtherSide) {
          break;
        } else {
          currentPulseUs = nextUs;
          movedServo     = true;
        }
      }
    } else if (state == TRK_CENTERING) {
      if (handSeen && centerFrames > 0) {
        int stepC  = 7;
        int nextUs = currentPulseUs + (int)trackDir * stepC;
        if (nextUs >= TRACK_PULSE_MIN_US && nextUs <= TRACK_PULSE_MAX_US) {
          currentPulseUs = nextUs;
          movedServo     = true;
        }
        centerFrames--;
      } else {
        state = TRK_LOCKED;
      }
    }

    if (movedServo) {
      pulseServoOnce(currentPulseUs);
    } else {
      digitalWrite(SERVO_PIN, LOW);
    }

    // La cara siempre refleja el ángulo físico exacto de currentPulseUs (-3..+3)
    updateLivingFace();

    unsigned long elapsedUs = micros() - frameStartUs;
    if (elapsedUs < 20000UL) {
      unsigned long remUs = 20000UL - elapsedUs;
      if (remUs > 1000UL) {
        delay(remUs / 1000UL);
      }
      delayMicroseconds(remUs % 1000UL);
    }
  }

  digitalWrite(SERVO_PIN, LOW);

  // Despedida simpática al retirar la mano (solo sonrisa "^_^" en brillo 8, sin guiño)
  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
  showRows(ICON_HAPPY);
  if (!waitInteractiveMs(550)) return;

  if (notifCount == 0) {
    resumeLivingFaceSmooth();
  }
}

void renderActiveIcon(const NotifItem& item) {
  unsigned long now = millis();
  setMatrixBrightness(BRIGHTNESS_NOTIF_ICON);

  if (item.type == NOTIF_MAIL) {
    showRows(ICON_EMAIL);
  } else if (item.type == NOTIF_CHAT) {
    if (lastNotifAnimMs == 0 || now - lastNotifAnimMs >= 420UL) {
      lastNotifAnimMs = now;
      notifAnimStep = (notifAnimStep + 1) % 3;
      if (notifAnimStep == 0) {
        showRows(ICON_CHAT_0);
      } else if (notifAnimStep == 1) {
        showRows(ICON_CHAT_1);
      } else {
        showRows(ICON_CHAT_2);
      }
    }
  } else if (item.type == NOTIF_MEET) {
    long remMin = currentMeetingRemainingMin(item);
    unsigned long tickMs = (remMin <= 0) ? 180UL : 340UL;
    if (lastNotifAnimMs == 0 || now - lastNotifAnimMs >= tickMs) {
      lastNotifAnimMs = now;
      notifAnimStep = (notifAnimStep + 1) % 4;
      if (notifAnimStep == 0) {
        showRows(ICON_MEET_0);
      } else if (notifAnimStep == 1) {
        showRows(ICON_MEET_1);
      } else if (notifAnimStep == 2) {
        showRows(ICON_MEET_2);
      } else {
        showRows(ICON_MEET_3);
      }
    }
  }
}

void buildNotifText(const NotifItem& item, uint8_t idx, uint8_t total, char* outBuf, size_t outSize) {
  char prefix[10] = "";
  if (total > 1) {
    snprintf(prefix, sizeof(prefix), "%u/%u ", (unsigned)(idx + 1), (unsigned)total);
  }

  if (item.type == NOTIF_MEET) {
    long remMin = currentMeetingRemainingMin(item);
    if (remMin > 0) {
      snprintf(outBuf, outSize, "%sEn %ldm: %s", prefix, remMin, item.sender);
    } else if (remMin == 0) {
      snprintf(outBuf, outSize, "%sInicia ya: %s", prefix, item.sender);
    } else {
      long pastMin = -remMin;
      if (pastMin <= 99) {
        snprintf(outBuf, outSize, "%sYa inici\x83 (%ldm): %s", prefix, pastMin, item.sender);
      } else {
        snprintf(outBuf, outSize, "%sYa inici\x83: %s", prefix, item.sender);
      }
    }
  } else if (item.count > 1) {
    snprintf(outBuf, outSize, "%s%s (x%u)", prefix, item.sender, (unsigned)item.count);
  } else {
    snprintf(outBuf, outSize, "%s%s", prefix, item.sender);
  }
}

void stopMotors() {
  analogWrite(MR_PWM_ALT, 0);
  analogWrite(ML_PWM, 0);
  analogWrite(MR_PWM, 0);
  digitalWrite(MR_CTRL, LOW);
  digitalWrite(ML_CTRL, LOW);
  digitalWrite(MR_PWM_ALT, LOW);
  digitalWrite(ML_PWM, LOW);
  digitalWrite(MR_PWM, LOW);
}

// --- Ejecución de Controles Directos desde el Chat (/robot) ---
void setMotorsRaw(bool leftForward, bool rightForward, uint8_t pwmVal) {
  if (motorFlipForward) {
    leftForward  = !leftForward;
    rightForward = !rightForward;
  }
  // En Keyestudio Turtle V3.x: LOW = avance, HIGH = retroceso
  digitalWrite(ML_CTRL, leftForward ? LOW : HIGH);
  digitalWrite(MR_CTRL, rightForward ? LOW : HIGH);
  analogWrite(ML_PWM, pwmVal);
  analogWrite(MR_PWM_ALT, pwmVal);
  analogWrite(MR_PWM, pwmVal);
}

void driveSafeWheelStep(const char* spec) {
  char dir = 'S';
  while (*spec == ' ' || *spec == ':') spec++;
  if (*spec != '\0') {
    dir = *spec;
    if (dir >= 'a' && dir <= 'z') dir = dir - 'a' + 'A';
  }

  if (dir == 'I') {
    motorFlipForward = !motorFlipForward;
    stopMotors();
    Serial.println(F("OK:W:FLIP"));
    return;
  }
  if (dir == 'S') {
    stopMotors();
    Serial.println(F("OK:W:STOP"));
    return;
  }

  uint16_t durMs = 220;
  const char* colon = strchr(spec, ':');
  if (colon != NULL) {
    int parsed = atoi(colon + 1);
    if (parsed >= 60 && parsed <= 900) {
      durMs = (uint16_t)parsed;
    } else if (parsed > 900) {
      durMs = 900;
    }
  }

  isSleeping = false;

  // Seguro previo anti-borde y anti-choque
  if (!rawSensorsOnDesk()) {
    stopMotors();
    setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
    showRows(ICON_LIFT_Oo);
    delay(550);
    if (notifCount == 0) resumeLivingFaceSmooth();
    Serial.println(F("OK:W:EDGE"));
    return;
  }
  if (dir == 'F' && readDistanceFastCm() <= 11.0f) {
    stopMotors();
    setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
    showRows(ICON_LIFT_Oo);
    delay(550);
    if (notifCount == 0) resumeLivingFaceSmooth();
    Serial.println(F("OK:W:OBSTACLE"));
    return;
  }

  bool lFwd = true;
  bool rFwd = true;
  uint8_t speedPwm = 165;
  if (dir == 'B') {
    lFwd = false;
    rFwd = false;
  } else if (dir == 'L') {
    lFwd = false;
    rFwd = true;
    speedPwm = 175;
  } else if (dir == 'R') {
    lFwd = true;
    rFwd = false;
    speedPwm = 175;
  } else if (dir != 'F') {
    stopMotors();
    Serial.println(F("OK:W:INVALID"));
    return;
  }

  setMotorsRaw(lFwd, rFwd, speedPwm);
  unsigned long t0 = millis();
  while (millis() - t0 < durMs) {
    if (!rawSensorsOnDesk()) {
      stopMotors();
      setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
      showRows(ICON_LIFT_Oo);
      delay(600);
      if (notifCount == 0) resumeLivingFaceSmooth();
      Serial.println(F("OK:W:EDGE"));
      return;
    }
    if (dir == 'F' && ((millis() - t0) % 40UL < 12UL) && readDistanceFastCm() <= 10.5f) {
      stopMotors();
      setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
      showRows(ICON_LIFT_Oo);
      delay(550);
      if (notifCount == 0) resumeLivingFaceSmooth();
      Serial.println(F("OK:W:OBSTACLE"));
      return;
    }
    delay(8);
  }
  stopMotors();
  if (notifCount == 0) {
    updateLivingFace();
  }
  Serial.println(F("OK:W:DONE"));
}

bool waitRemoteGestureMs(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    pollBluetooth();
    if (isRobotLifted()) {
      handleLiftedReaction();
      return false;
    }
    if (pendingRemoteKind != RCMD_NONE) {
      return false;
    }
    delay(20);
  }
  return true;
}

void executeNeckRemoteCmd(const char* arg) {
  isSleeping = false;
  char c0 = arg[0];
  if (c0 >= 'a' && c0 <= 'z') c0 = c0 - 'a' + 'A';

  if (c0 == 'L') {
    panSmoothTo(PULSE_MIN_US, false, (notifCount == 0));
    Serial.println(F("OK:N:L"));
  } else if (c0 == 'R') {
    panSmoothTo(PULSE_MAX_US, false, (notifCount == 0));
    Serial.println(F("OK:N:R"));
  } else if (c0 == 'C') {
    panSmoothTo(PULSE_CENTER_US, false, (notifCount == 0));
    Serial.println(F("OK:N:C"));
  } else if (c0 == 'N') {
    // Negar con la cabeza ("NO")
    int prev = currentPulseUs;
    panSmoothTo(PULSE_CENTER_US - 220, false, (notifCount == 0));
    panSmoothTo(PULSE_CENTER_US + 220, false, (notifCount == 0));
    panSmoothTo(PULSE_CENTER_US - 180, false, (notifCount == 0));
    panSmoothTo(prev, false, (notifCount == 0));
    Serial.println(F("OK:N:NO"));
  } else if (c0 == 'S') {
    // Escaneo curioso completo ("SCAN")
    panSmoothTo(PULSE_MIN_US, false, (notifCount == 0));
    waitRemoteGestureMs(220);
    panSmoothTo(PULSE_MAX_US, false, (notifCount == 0));
    waitRemoteGestureMs(220);
    panSmoothTo(PULSE_CENTER_US, false, (notifCount == 0));
    Serial.println(F("OK:N:SCAN"));
  } else {
    int deg = atoi(arg);
    if (deg < 40)  deg = 40;
    if (deg > 140) deg = 140;
    int targetUs = (int)map(deg, 40, 140, TRACK_PULSE_MIN_US, TRACK_PULSE_MAX_US);
    panSmoothTo(targetUs, false, (notifCount == 0));
    Serial.println(F("OK:N:ANG"));
  }

  pauseStartMs       = millis();
  nextObservePauseMs = pickRandomObservePauseMs();
}

void executeGestureRemoteCmd(const char* arg) {
  char c0 = arg[0];
  char c1 = (arg[0] != '\0') ? arg[1] : '\0';
  if (c0 >= 'a' && c0 <= 'z') c0 = c0 - 'a' + 'A';
  if (c1 >= 'a' && c1 <= 'z') c1 = c1 - 'a' + 'A';

  setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);

  if (c0 == 'S' && c1 == 'L') {
    // SLEEP (Modo Dormir)
    isSleeping = true;
    showRowsP(ICON_SLEEP_0);
    Serial.println(F("OK:G:SLEEP"));
    return;
  }
  if (c0 == 'W') {
    // WAKE (Despertar)
    isSleeping = false;
    resumeLivingFaceSmooth();
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
    Serial.println(F("OK:G:WAKE"));
    return;
  }

  isSleeping = false;

  if (c0 == 'H' && c1 == 'E') {
    // HEART (Corazón latiendo)
    for (uint8_t i = 0; i < 3; i++) {
      showRowsP(ICON_HEART_BIG);
      if (!waitRemoteGestureMs(360)) return;
      showRowsP(ICON_HEART_SMALL);
      if (!waitRemoteGestureMs(240)) return;
    }
    showRowsP(ICON_HEART_BIG);
    if (!waitRemoteGestureMs(400)) return;
    Serial.println(F("OK:G:HEART"));
  } else if (c0 == 'S') {
    // SURPRISE (O_o <-> o_O)
    for (uint8_t i = 0; i < 4; i++) {
      showRows((i % 2 == 0) ? ICON_LIFT_Oo : ICON_LIFT_oO);
      if (!waitRemoteGestureMs(420)) return;
    }
    Serial.println(F("OK:G:SURPRISE"));
  } else if (c0 == 'T') {
    // THINK (Pensando...)
    for (uint8_t i = 0; i < 2; i++) {
      showRowsP(ICON_THINK_0);
      if (!waitRemoteGestureMs(320)) return;
      showRowsP(ICON_THINK_1);
      if (!waitRemoteGestureMs(320)) return;
      showRowsP(ICON_THINK_2);
      if (!waitRemoteGestureMs(420)) return;
    }
    Serial.println(F("OK:G:THINK"));
  } else if (c0 == 'C') {
    // CELEBRATE (^_^ + baile alegre de cuello)
    showRows(ICON_HAPPY);
    panSmoothTo(PULSE_CENTER_US - 240, false, false);
    panSmoothTo(PULSE_CENTER_US + 240, false, false);
    panSmoothTo(PULSE_CENTER_US, false, false);
    if (!waitRemoteGestureMs(500)) return;
    Serial.println(F("OK:G:CELEBRATE"));
  } else {
    // HAPPY (^_^)
    showRows(ICON_HAPPY);
    if (!waitRemoteGestureMs(1800)) return;
    Serial.println(F("OK:G:HAPPY"));
  }

  if (notifCount == 0 || notifPhase == PHASE_REST) {
    resumeLivingFaceSmooth();
    if (notifPhase == PHASE_REST) {
      phaseStartMs = millis();
    }
  }
  pauseStartMs       = millis();
  nextObservePauseMs = pickRandomObservePauseMs();
}

void executePendingRemoteCommand() {
  RemoteCmdKind kind = pendingRemoteKind;
  if (kind == RCMD_NONE) return;

  char argCopy[36];
  strncpy(argCopy, pendingRemotePayload, sizeof(argCopy) - 1);
  argCopy[sizeof(argCopy) - 1] = '\0';
  pendingRemoteKind = RCMD_NONE;

  stopMotors();

  if (kind == RCMD_WHEEL) {
    driveSafeWheelStep(argCopy);
  } else if (kind == RCMD_NECK) {
    executeNeckRemoteCmd(argCopy);
  } else if (kind == RCMD_GESTURE) {
    executeGestureRemoteCmd(argCopy);
  } else if (kind == RCMD_TEXT) {
    isSleeping = false;
    if (argCopy[0] != '\0') {
      scrollText(argCopy, false, false);
    }
    if ((notifCount == 0 || notifPhase == PHASE_REST) && pendingRemoteKind == RCMD_NONE) {
      resumeLivingFaceSmooth();
      if (notifPhase == PHASE_REST) {
        phaseStartMs = millis();
      }
    }
    Serial.println(F("OK:T"));
  }
}

void setup() {
  pinMode(MR_CTRL, OUTPUT);
  pinMode(ML_CTRL, OUTPUT);
  pinMode(MR_PWM_ALT, OUTPUT);
  pinMode(ML_PWM, OUTPUT);
  pinMode(MR_PWM, OUTPUT);
  stopMotors();

  pinMode(SERVO_PIN, OUTPUT);
  digitalWrite(SERVO_PIN, LOW);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(LINE_L_PIN, INPUT);
  pinMode(LINE_M_PIN, INPUT);
  pinMode(LINE_R_PIN, INPUT);
  deskCalibrated = rawSensorsOnDesk();

  randomSeed((unsigned long)analogRead(A0) ^ ((unsigned long)analogRead(A1) << 8) ^ micros());

  Serial.begin(9600);
  ht16k33Init();
  clearScreen();
  resumeLivingFaceSmooth(0);

  for (int i = 0; i < 6; i++) {
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(currentPulseUs);
    digitalWrite(SERVO_PIN, LOW);
    updateLivingFace();
    delay(20);
  }
  panSmoothTo(pickRandomTargetUs(), false, true);

  pauseStartMs       = millis();
  nextObservePauseMs = pickRandomObservePauseMs();
}

void loop() {
  static unsigned long lastIdleSonarMs = 0;

  stopMotors();
  pollBluetooth();

  // Si alguien levanta el robot de la mesa -> carita "O_o" / "o_O" inmediata
  if (isRobotLifted()) {
    handleLiftedReaction();
    return;
  }

  // Si llega un comando directo desde /robot en Antigravity, ejecutarlo de inmediato
  if (pendingRemoteKind != RCMD_NONE) {
    executePendingRemoteCommand();
    return;
  }

  if (triggerRemoteBuenDia && notifCount == 0) {
    triggerRemoteBuenDia = false;
    isSleeping = false;
    scrollText(MSG_BUEN_DIA, false, true);
    if (notifCount == 0 && pendingRemoteKind == RCMD_NONE) {
      resumeLivingFaceSmooth(0);
    }
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
    return;
  }

  // ==========================================================================
  // MODO COLA DE NOTIFICACIONES (Mail ✉ / Chat 💬 / Reuniones ⏰ + Carita Viva)
  // ==========================================================================
  if (notifCount > 0) {
    if (currentNotifIdx >= notifCount) {
      currentNotifIdx = 0;
    }

    // Fase de descanso entre recordatorios: muestra la carita viva y checkea mano (<= 10 cm)
    if (notifPhase == PHASE_REST) {
      updateLivingFace();

      if (millis() - lastIdleSonarMs >= 60UL) {
        lastIdleSonarMs = millis();
        if (isSomeoneNear()) {
          dismissCurrentNotification();
          return;
        }
      }

      if (millis() - phaseStartMs >= NOTIF_REST_MS) {
        beginNotifPhaseIcon((currentNotifIdx + 1) % notifCount, true);
      } else {
        delay(20);
      }
      return;
    }

    if (notifNeedsCenterPan || notifNeedsAttentionWiggle) {
      setMatrixBrightness(BRIGHTNESS_NOTIF_ICON);
      NotifType t = notifQueue[currentNotifIdx].type;
      if (t == NOTIF_MAIL) {
        showRows(ICON_EMAIL);
      } else if (t == NOTIF_MEET) {
        showRows(ICON_MEET_0);
      } else {
        showRows(ICON_CHAT_2);
      }

      if (notifNeedsAttentionWiggle) {
        notifNeedsAttentionWiggle = false;
        notifNeedsCenterPan       = false;
        panSmoothTo(PULSE_CENTER_US - 95, false, false);
        panSmoothTo(PULSE_CENTER_US + 95, false, false);
        panSmoothTo(PULSE_CENTER_US, false, false);
      } else {
        notifNeedsCenterPan = false;
        panSmoothTo(PULSE_CENTER_US, false, false);
      }
      phaseStartMs    = millis();
      lastNotifAnimMs = 0;
    }

    if (isSomeoneNear()) {
      dismissCurrentNotification();
      return;
    }

    if (notifPhase == PHASE_ICON) {
      renderActiveIcon(notifQueue[currentNotifIdx]);

      if (millis() - phaseStartMs >= ICON_HOLD_MS) {
        notifPhase = PHASE_TEXT;
      } else {
        delay(35);
      }
      return;
    }

    char displayLine[60];
    buildNotifText(
      notifQueue[currentNotifIdx],
      currentNotifIdx,
      notifCount,
      displayLine,
      sizeof(displayLine)
    );

    bool handDetectedDuringScroll = scrollText(displayLine, true, false);
    if (handDetectedDuringScroll) {
      dismissCurrentNotification();
      return;
    }

    if (notifCount > 0) {
      // Volver a mostrar la carita viva durante NOTIF_REST_MS antes de repetir el siguiente aviso
      notifPhase      = PHASE_REST;
      phaseStartMs    = millis();
      lastNotifAnimMs = 0;
      notifAnimStep   = 0;
      resumeLivingFaceSmooth(0);
    } else {
      resumeLivingFaceSmooth(0);
    }
    return;
  }

  // ==========================================================================
  // MODO DORMIR (/robot dormir -> ojos cerrados apacibles y cuello quieto)
  // ==========================================================================
  if (isSleeping) {
    setMatrixBrightness(BRIGHTNESS_TEXT_GESTURE);
    if (((millis() / 1300UL) % 2UL) == 0UL) {
      showRowsP(ICON_SLEEP_0);
    } else {
      showRowsP(ICON_SLEEP_1);
    }
    if (millis() - lastIdleSonarMs >= 120UL) {
      lastIdleSonarMs = millis();
      if (isSomeoneNear()) {
        isSleeping = false;
        resumeLivingFaceSmooth();
        pauseStartMs       = millis();
        nextObservePauseMs = pickRandomObservePauseMs();
      }
    }
    delay(40);
    return;
  }

  // ==========================================================================
  // ESTADO VIVO PERMANENTE (Cara sincronizada al ángulo + Giro 5~30 s)
  // ==========================================================================
  updateLivingFace();

  if (millis() - lastIdleSonarMs >= 55UL) {
    lastIdleSonarMs = millis();
    if (isHandReadyToTrack()) {
      trackHandWithEyes();
      pauseStartMs       = millis();
      nextObservePauseMs = pickRandomObservePauseMs();
      return;
    }
  }

  if (millis() - pauseStartMs >= nextObservePauseMs) {
    int randomTargetUs = pickRandomTargetUs();
    bool spottedSomeone = panSmoothTo(randomTargetUs, true, true);

    if (spottedSomeone && notifCount == 0) {
      trackHandWithEyes();
    }

    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
    return;
  }

  delay(16);
}

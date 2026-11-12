// ============================================================================
// Keyestudio Smart Little Turtle V3.1
// Observador Silencioso + "Buen día!" (<=10 cm) + Cola de Notificaciones BLE
// ============================================================================
// 1) ESTADO DE REPOSO:
//    - Pantalla 8x8 apagada y motores bloqueados en LOW.
//    - Giro suave aleatorio cada 30 segundos (~52° a ~128°, 50 Hz sin ruido).
//    - Si pasas la mano bien cerca del sensor (<= 10 cm), desplaza "Buen día!"
//      de Derecha a Izquierda.
//
// 2) COLA DE NOTIFICACIONES BLUETOOTH DX-BT24 (UART 9600 bps en RX/TX):
//    - Formato recibido desde PC:
//        "M:<Remitente>\n" -> Encola aviso de EMAIL (✉) + nombre del remitente
//        "C:<Remitente>\n" -> Encola aviso de CHAT  (💬) + nombre del remitente
//        "X\n"             -> Limpia toda la cola
//        "B\n"             -> Saludo remoto "Buen día!"
//    - Al tener 1 o varios mensajes en cola:
//        * Mira al frente (90°).
//        * Muestra el ícono animado (✉ o 💬) unos segundos (~3.2 s) y luego
//          desplaza el nombre de quien envió el mensaje (ej. "1/2 Sylvia").
//        * Si hay varios mensajes, los va iterando uno tras otro continuamente.
//        * Al acercar la mano al sensor (<= 10 cm), muestra ✔ y borra de a UNO
//          el mensaje actual, pasando al siguiente hasta vaciar la cola.
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
const int SERVO_PIN       = 10;
const int PULSE_MIN_US    = 1070; // ~52° derecha
const int PULSE_CENTER_US = 1490; // ~90° frente
const int PULSE_MAX_US    = 1910; // ~128° izquierda
const int PULSE_STEP_US   = 7;    // Velocidad suave
const int MIN_DELTA_US    = 260;  // Mínimo ~24° por giro aleatorio

// --- Tiempos ---
const unsigned long OBSERVE_MIN_MS = 20000UL; // Pausa aleatoria mín: 20 s
const unsigned long OBSERVE_MAX_MS = 30000UL; // Pausa aleatoria máx: 30 s
const unsigned long ICON_HOLD_MS   = 3200UL;  // Segundos mostrando ícono antes del nombre
const unsigned int  SCROLL_STEP_MS = 70;      // Velocidad de desplazamiento de texto

// --- Sensor Ultrasónico (proximidad corta: ~10 cm) ---
const int   TRIG_PIN       = 12;
const int   ECHO_PIN       = 13;
const float NEAR_DETECT_CM = 10.0f;

// --- Matriz 8x8 HT16K33 (I2C 0x70) ---
const uint8_t HT16K33_ADDR = 0x70;

// --- Cola de Notificaciones ---
enum NotifType : uint8_t {
  NOTIF_NONE = 0,
  NOTIF_MAIL = 1,
  NOTIF_CHAT = 2
};

enum NotifPhase : uint8_t {
  PHASE_ICON = 0,
  PHASE_TEXT = 1
};

const uint8_t MAX_NOTIFS     = 10;
const uint8_t MAX_SENDER_LEN = 26;

struct NotifItem {
  NotifType type;
  char sender[MAX_SENDER_LEN];
};

NotifItem notifQueue[MAX_NOTIFS];
uint8_t notifCount      = 0;
uint8_t currentNotifIdx = 0;
NotifPhase notifPhase   = PHASE_ICON;

bool notifNeedsCenterPan      = false;
unsigned long phaseStartMs    = 0;
unsigned long lastNotifAnimMs = 0;
int notifAnimStep             = 0;

// Buffer de recepción serie no bloqueante
char rxBuf[44];
uint8_t rxLen = 0;
bool triggerRemoteBuenDia = false;

int currentPulseUs                = PULSE_CENTER_US;
unsigned long pauseStartMs        = 0;
unsigned long nextObservePauseMs  = 25000UL;

unsigned long pickRandomObservePauseMs() {
  return (unsigned long)random(OBSERVE_MIN_MS, OBSERVE_MAX_MS + 1UL);
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

const uint8_t ICON_CHECK[8] = {
  0x00, 0x80, 0xC0, 0x60, 0x31, 0x1B, 0x0E, 0x04
};

// ============================================================================
// FUENTE PROPORCIONAL 8 FILAS (bit 0 = arriba y=0, bit 7 = abajo y=7)
// Códigos extendidos desde PC:
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
  {0x80, 4, {0x40, 0xA8, 0xAA, 0xF1, 0}},       // á
  {0x81, 4, {0x70, 0xA8, 0xAA, 0xB1, 0}},       // é
  {0x82, 3, {0x88, 0xFA, 0x81, 0, 0}},          // í
  {0x83, 4, {0x70, 0x88, 0x8A, 0x71, 0}},       // ó
  {0x84, 4, {0x78, 0x80, 0x82, 0xF9, 0}},       // ú
  {0x85, 4, {0xFA, 0x09, 0x0A, 0xF1, 0}},       // ñ
  {0x86, 5, {0xFC, 0x09, 0x11, 0x21, 0xFC}},    // Ñ
  {0x87, 4, {0x79, 0x80, 0x80, 0xF9, 0}},       // ü

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

// Texto "Buen día!" usando el código 0x82 para 'í'
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
  // Carácter desconocido -> pequeño guion
  outCols[0] = 0x10;
  outCols[1] = 0x10;
  return 2;
}

// --- Funciones de Matriz HT16K33 ---
void setMatrixBrightness(uint8_t level) {
  if (level > 15) level = 15;
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

  setMatrixBrightness(8);
}

void showRows(const uint8_t rowsTopToBottom[8]) {
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x00);
  for (int r = 0; r < 8; r++) {
    Wire.write(rowsTopToBottom[7 - r]);
    Wire.write(0x00);
  }
  Wire.endTransmission();
}

void clearScreen() {
  setMatrixBrightness(8);
  const uint8_t empty[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  showRows(empty);
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

// --- Gestión de Cola de Notificaciones ---
void enqueueNotification(NotifType type, const char* senderRaw) {
  bool wasEmpty = (notifCount == 0);

  // Si la cola está llena, descartar la más antigua para hacer lugar
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
  item.type = type;

  // Copiar remitente limpio (o texto por defecto si viene vacío)
  while (*senderRaw == ' ' || *senderRaw == ':') {
    senderRaw++;
  }
  if (*senderRaw == '\0') {
    strncpy(item.sender, (type == NOTIF_MAIL) ? "Correo nuevo" : "Mensaje nuevo", MAX_SENDER_LEN - 1);
  } else {
    strncpy(item.sender, senderRaw, MAX_SENDER_LEN - 1);
  }
  item.sender[MAX_SENDER_LEN - 1] = '\0';

  notifCount++;

  if (wasEmpty) {
    currentNotifIdx     = 0;
    notifPhase          = PHASE_ICON;
    phaseStartMs        = millis();
    lastNotifAnimMs     = 0;
    notifAnimStep       = 0;
    notifNeedsCenterPan = true;
  }
}

void processBluetoothLine(char* line) {
  // Quitar espacios/retornos al final
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
  } else if (cmd == 'X' || cmd == 'x') {
    notifCount = 0;
    currentNotifIdx = 0;
    clearScreen();
    Serial.println(F("OK:CLR"));
  } else if (cmd == 'B' || cmd == 'b') {
    triggerRemoteBuenDia = true;
    Serial.println(F("OK:BUENDIA"));
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

// --- Desplazamiento de cualquier texto por la matriz 8x8 ---
// Devuelve true si el usuario acercó la mano (<= 10 cm) durante el scroll
bool scrollText(const char* text, bool abortOnNear, bool abortOnNewNotifWhenIdle) {
  // Expandir texto en arreglo temporal de columnas (máx ~190 columnas)
  uint8_t cols[196];
  int totalCols = 0;

  for (int i = 0; text[i] != '\0' && totalCols < (int)sizeof(cols) - 6; i++) {
    uint8_t gCols[5];
    uint8_t w = getGlyph((uint8_t)text[i], gCols);
    for (uint8_t c = 0; c < w && totalCols < (int)sizeof(cols); c++) {
      cols[totalCols++] = gCols[c];
    }
    // 1 columna de espacio entre letras (salvo que ya sea un espacio)
    if (text[i] != ' ' && text[i + 1] != '\0' && text[i + 1] != ' ' && totalCols < (int)sizeof(cols)) {
      cols[totalCols++] = 0x00;
    }
  }

  setMatrixBrightness(8);

  for (int offset = -7; offset <= totalCols; offset++) {
    pollBluetooth();

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

// --- Descarte de a UNO al acercar la mano (<= 10 cm) ---
void dismissCurrentNotification() {
  if (notifCount == 0) return;

  // Eliminar únicamente la notificación actual de la cola
  for (uint8_t i = currentNotifIdx; i + 1 < notifCount; i++) {
    notifQueue[i] = notifQueue[i + 1];
  }
  notifCount--;

  // Mostrar tilde de confirmación (✔)
  setMatrixBrightness(10);
  showRows(ICON_CHECK);

  // Mantener ✔ al menos 650 ms y esperar a que retire la mano (> 14 cm)
  // para que un solo gesto borre exactamente UN mensaje y no varios seguidos
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
    if (currentNotifIdx >= notifCount) {
      currentNotifIdx = 0;
    }
    notifPhase      = PHASE_ICON;
    phaseStartMs    = millis();
    lastNotifAnimMs = 0;
    notifAnimStep   = 0;
  } else {
    currentNotifIdx = 0;
    clearScreen();
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

bool panSmoothTo(int targetPulseUs, bool stopOnNear = true) {
  int nearHits = 0;

  while (currentPulseUs != targetPulseUs) {
    unsigned long frameStartUs = micros();

    pollBluetooth();
    if (stopOnNear && notifCount > 0) {
      digitalWrite(SERVO_PIN, LOW);
      return false;
    }

    int diff = targetPulseUs - currentPulseUs;
    if (abs(diff) <= PULSE_STEP_US) {
      currentPulseUs = targetPulseUs;
    } else if (diff > 0) {
      currentPulseUs += PULSE_STEP_US;
    } else {
      currentPulseUs -= PULSE_STEP_US;
    }

    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(currentPulseUs);
    digitalWrite(SERVO_PIN, LOW);

    if (stopOnNear) {
      if (readDistanceFastCm() <= NEAR_DETECT_CM) {
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
  return false;
}

void renderActiveIcon(NotifType type) {
  unsigned long now = millis();

  if (type == NOTIF_MAIL) {
    showRows(ICON_EMAIL);
    if (now - lastNotifAnimMs >= 120UL) {
      lastNotifAnimMs = now;
      notifAnimStep = (notifAnimStep + 1) % 20;
      uint8_t tri = (notifAnimStep <= 10) ? notifAnimStep : (20 - notifAnimStep);
      setMatrixBrightness(2 + tri);
    }
  } else if (type == NOTIF_CHAT) {
    setMatrixBrightness(9);
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
  }
}

void stopMotors() {
  digitalWrite(MR_CTRL, LOW);
  digitalWrite(ML_CTRL, LOW);
  digitalWrite(MR_PWM_ALT, LOW);
  digitalWrite(ML_PWM, LOW);
  digitalWrite(MR_PWM, LOW);
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

  randomSeed((unsigned long)analogRead(A0) ^ ((unsigned long)analogRead(A1) << 8) ^ micros());

  Serial.begin(9600);
  ht16k33Init();
  clearScreen();

  for (int i = 0; i < 6; i++) {
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(currentPulseUs);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  panSmoothTo(pickRandomTargetUs(), false);

  pauseStartMs       = millis();
  nextObservePauseMs = pickRandomObservePauseMs();
}

void loop() {
  stopMotors();
  pollBluetooth();

  // Saludo remoto disparado por Bluetooth ('B')
  if (triggerRemoteBuenDia && notifCount == 0) {
    triggerRemoteBuenDia = false;
    scrollText(MSG_BUEN_DIA, false, true);
    clearScreen();
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
    return;
  }

  // ==========================================================================
  // MODO COLA DE NOTIFICACIONES (Ícono -> Nombre remitente -> Siguiente...)
  // ==========================================================================
  if (notifCount > 0) {
    if (currentNotifIdx >= notifCount) {
      currentNotifIdx = 0;
    }

    if (notifNeedsCenterPan) {
      notifNeedsCenterPan = false;
      if (notifQueue[currentNotifIdx].type == NOTIF_MAIL) {
        showRows(ICON_EMAIL);
      } else {
        showRows(ICON_CHAT_2);
      }
      panSmoothTo(PULSE_CENTER_US, false);
      phaseStartMs    = millis();
      lastNotifAnimMs = 0;
    }

    // Si acercas la mano a <= 10 cm, borra el mensaje actual y pasa al siguiente
    if (isSomeoneNear()) {
      dismissCurrentNotification();
      return;
    }

    if (notifPhase == PHASE_ICON) {
      renderActiveIcon(notifQueue[currentNotifIdx].type);

      if (millis() - phaseStartMs >= ICON_HOLD_MS) {
        notifPhase = PHASE_TEXT;
      } else {
        delay(35);
      }
      return;
    }

    // PHASE_TEXT: armar etiqueta ("Sylvia" o "1/3 Sylvia" si hay varios)
    char displayLine[36];
    if (notifCount > 1) {
      snprintf(
        displayLine,
        sizeof(displayLine),
        "%u/%u %s",
        (unsigned)(currentNotifIdx + 1),
        (unsigned)notifCount,
        notifQueue[currentNotifIdx].sender
      );
    } else {
      snprintf(displayLine, sizeof(displayLine), "%s", notifQueue[currentNotifIdx].sender);
    }

    bool handDetectedDuringScroll = scrollText(displayLine, true, false);
    if (handDetectedDuringScroll) {
      dismissCurrentNotification();
      return;
    }

    // Pasar al siguiente mensaje de la cola (o repetir si solo queda 1)
    if (notifCount > 0) {
      currentNotifIdx = (currentNotifIdx + 1) % notifCount;
      notifPhase      = PHASE_ICON;
      phaseStartMs    = millis();
      lastNotifAnimMs = 0;
      notifAnimStep   = 0;
    }
    return;
  }

  // ==========================================================================
  // ESTADO DE REPOSO (Pantalla oscura + "Buen día!" <= 10 cm + Giro 20~30 s)
  // ==========================================================================
  if (isSomeoneNear()) {
    digitalWrite(SERVO_PIN, LOW);
    randomSeed(random(100000L) ^ micros());
    scrollText(MSG_BUEN_DIA, false, true);
    if (notifCount == 0) {
      clearScreen();
      delay(600);
    }
    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
    return;
  }

  if (millis() - pauseStartMs >= nextObservePauseMs) {
    int randomTargetUs = pickRandomTargetUs();
    bool spottedSomeone = panSmoothTo(randomTargetUs, true);

    if (spottedSomeone && notifCount == 0) {
      scrollText(MSG_BUEN_DIA, false, true);
      if (notifCount == 0) {
        clearScreen();
        delay(600);
      }
    }

    pauseStartMs       = millis();
    nextObservePauseMs = pickRandomObservePauseMs();
  }

  delay(50);
}

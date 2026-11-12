// ============================================================================
// Keyestudio Smart Little Turtle V3.1
// Observador Silencioso + "Buen día!" + Notificaciones Bluetooth (DX-BT24)
// ============================================================================
// 1) ESTADO DE REPOSO (en paralelo):
//    - Pantalla 8x8 apagada.
//    - Giro suave aleatorio cada 30 segundos (~52° a ~128°, 50 Hz sin ruido).
//    - Si alguien se acerca (<= 35 cm), pasa "Buen día!" de Derecha a Izquierda.
//
// 2) NOTIFICACIONES VÍA BLUETOOTH DX-BT24 (UART 9600 bps en RX/TX):
//    - Recibe 'M' (ej. "MAIL\n") -> Mira al frente y muestra SOBRE DE EMAIL (✉)
//      con brillo respirando suave.
//    - Recibe 'H' o 'C' (ej. "CHAT\n") -> Mira al frente y muestra GLOBITO DE
//      CHAT (💬) animando las líneas de mensaje.
//    - La notificación queda avisando hasta que:
//        a) Acerques la mano (<= 35 cm) -> Muestra ✔ de "visto" y se apaga, o
//        b) Pasen 3 minutos (180 s) -> Se apaga sola y vuelve al reposo, o
//        c) Reciba 'X' ("CLR\n") por Bluetooth.
// ============================================================================

#include <Wire.h>

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
const int PULSE_STEP_US   = 7;    // Velocidad suave aprobada
const int MIN_DELTA_US    = 260;  // Mínimo ~24° por giro aleatorio

// --- Tiempos ---
const unsigned long OBSERVE_PAUSE_MS = 30000UL;  // Giro aleatorio cada 30 s
const unsigned long NOTIF_TIMEOUT_MS = 180000UL; // 3 minutos máx de aviso
const unsigned int  SCROLL_STEP_MS   = 75;

// --- Sensor Ultrasónico ---
const int   TRIG_PIN       = 12;
const int   ECHO_PIN       = 13;
const float NEAR_DETECT_CM = 35.0f;

// --- Matriz 8x8 HT16K33 (I2C 0x70) ---
const uint8_t HT16K33_ADDR = 0x70;

// --- Estados de Notificación ---
enum NotifType : uint8_t {
  NOTIF_NONE = 0,
  NOTIF_MAIL = 1,
  NOTIF_CHAT = 2
};

NotifType pendingNotif = NOTIF_NONE;
bool notifNeedsCenterPan = false;
unsigned long notifStartMs = 0;
unsigned long lastNotifAnimMs = 0;
int notifAnimStep = 0;

int currentPulseUs = PULSE_CENTER_US;
unsigned long pauseStartMs = 0;

// ============================================================================
// ÍCONOS 8x8 (fila 0 = arriba .. fila 7 = abajo; bit 0 = izq .. bit 7 = der)
// ============================================================================

// Sobre de Email (✉)
const uint8_t ICON_EMAIL[8] = {
  0x00, // ........
  0xFF, // ********
  0xC3, // **....**
  0xA5, // *.*..*.*
  0x99, // *..**..*
  0x81, // *......*
  0xFF, // ********
  0x00  // ........
};

// Globito de Chat / Hangouts (💬) - 3 cuadros escribiendo
const uint8_t ICON_CHAT_0[8] = {
  0x7E, // .******.
  0x81, // *......*
  0x81, // *......*
  0x81, // *......*
  0x81, // *......*
  0x72, // .*..***.
  0x0A, // .*.*....
  0x06  // .**.....
};

const uint8_t ICON_CHAT_1[8] = {
  0x7E, // .******.
  0x81, // *......*
  0xBD, // *.****.*
  0x81, // *......*
  0x81, // *......*
  0x72, // .*..***.
  0x0A, // .*.*....
  0x06  // .**.....
};

const uint8_t ICON_CHAT_2[8] = {
  0x7E, // .******.
  0x81, // *......*
  0xBD, // *.****.*
  0x81, // *......*
  0x9D, // *.***..*
  0x72, // .*..***.
  0x0A, // .*.*....
  0x06  // .**.....
};

// Tilde de "Visto" (✔) al pasar la mano para descartar notificación
const uint8_t ICON_CHECK[8] = {
  0x00, // ........
  0x80, // .......*
  0xC0, // ......**
  0x60, // .....**.
  0x31, // *...**..
  0x1B, // **.**...
  0x0E, // .***....
  0x04  // ..*.....
};

// ============================================================================
// TEXTO "Buen día!" EN COLUMNAS VERTICALES
// ============================================================================
const uint8_t BANNER_BUEN_DIA[] = {
  0xFE, 0x92, 0x92, 0x92, 0x6C, 0x00, // B
  0x78, 0x80, 0x80, 0xF8,       0x00, // u
  0x70, 0xA8, 0xA8, 0xB0,       0x00, // e
  0xF8, 0x08, 0x08, 0xF0,             // n
  0x00, 0x00, 0x00,                   // (espacio)
  0x70, 0x88, 0x88, 0xFE,       0x00, // d
  0x88, 0xFA, 0x81,             0x00, // í
  0x40, 0xA8, 0xA8, 0xF0,       0x00, // a
  0xBE                                // !
};
const int BANNER_LEN = sizeof(BANNER_BUEN_DIA) / sizeof(BANNER_BUEN_DIA[0]);

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

// --- Lectura de Comandos Bluetooth (DX-BT24 a 9600 bps) ---
bool triggerRemoteBuenDia = false;

void pollBluetooth() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == 'M' || c == 'm') {
      pendingNotif = NOTIF_MAIL;
      notifNeedsCenterPan = true;
      notifStartMs = millis();
      notifAnimStep = 0;
      Serial.println(F("OK:MAIL"));
    } else if (c == 'H' || c == 'h' || c == 'C' || c == 'c') {
      pendingNotif = NOTIF_CHAT;
      notifNeedsCenterPan = true;
      notifStartMs = millis();
      notifAnimStep = 0;
      Serial.println(F("OK:CHAT"));
    } else if (c == 'X' || c == 'x') {
      pendingNotif = NOTIF_NONE;
      clearScreen();
      Serial.println(F("OK:CLR"));
    } else if (c == 'B' || c == 'b') {
      triggerRemoteBuenDia = true;
      Serial.println(F("OK:BUENDIA"));
    }
  }
}

void scrollBuenDia() {
  setMatrixBrightness(8);
  for (int offset = -7; offset <= BANNER_LEN; offset++) {
    pollBluetooth();
    if (pendingNotif != NOTIF_NONE) {
      return; // Si justo entra una notificación importante, priorizarla
    }

    uint8_t rows[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int x = 0; x < 8; x++) {
      int colIdx = offset + x;
      if (colIdx >= 0 && colIdx < BANNER_LEN) {
        uint8_t colBits = BANNER_BUEN_DIA[colIdx];
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

  clearScreen();
}

// --- Lectura Ultrasónica rápida ---
float readDistanceFastCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 5000UL);
  if (duration == 0) {
    return 999.0f;
  }
  return (duration * 0.0343f) / 2.0f;
}

bool isSomeoneNear() {
  if (readDistanceFastCm() > NEAR_DETECT_CM) {
    return false;
  }
  delay(8);
  return (readDistanceFastCm() <= NEAR_DETECT_CM + 3.0f);
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

// --- Giro continuo a 50 Hz exactos (20,000 µs) ---
bool panSmoothTo(int targetPulseUs, bool stopOnNear = true) {
  int nearHits = 0;

  while (currentPulseUs != targetPulseUs) {
    unsigned long frameStartUs = micros();

    pollBluetooth();
    if (stopOnNear && pendingNotif != NOTIF_NONE) {
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

// --- Renderizado animado de la Notificación Activa ---
void updateNotificationDisplay() {
  unsigned long now = millis();

  if (pendingNotif == NOTIF_MAIL) {
    showRows(ICON_EMAIL);
    // Efecto suave de "respiración" de luz (niveles 2..12) cada 120 ms
    if (now - lastNotifAnimMs >= 120UL) {
      lastNotifAnimMs = now;
      notifAnimStep = (notifAnimStep + 1) % 20;
      uint8_t tri = (notifAnimStep <= 10) ? notifAnimStep : (20 - notifAnimStep);
      setMatrixBrightness(2 + tri); // Brillo 2 a 12 suave
    }
  } else if (pendingNotif == NOTIF_CHAT) {
    setMatrixBrightness(9);
    if (now - lastNotifAnimMs >= 450UL) {
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

  // 9600 bps coincide con la velocidad de fábrica del Bluetooth DX-BT24
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

  pauseStartMs = millis();
}

void loop() {
  stopMotors();
  pollBluetooth();

  // Saludo remoto disparado por Bluetooth ('B')
  if (triggerRemoteBuenDia && pendingNotif == NOTIF_NONE) {
    triggerRemoteBuenDia = false;
    scrollBuenDia();
    pauseStartMs = millis();
    return;
  }

  // ==========================================================================
  // MODO PARALELO DE NOTIFICACIÓN ACTIVA (Email o Hangouts/Chat)
  // ==========================================================================
  if (pendingNotif != NOTIF_NONE) {
    // Al llegar notificación, mostrar ícono al instante y girar suave a mirarte al frente
    if (notifNeedsCenterPan) {
      notifNeedsCenterPan = false;
      if (pendingNotif == NOTIF_MAIL) {
        showRows(ICON_EMAIL);
      } else {
        showRows(ICON_CHAT_2);
      }
      panSmoothTo(PULSE_CENTER_US, false);
      lastNotifAnimMs = 0;
    }

    updateNotificationDisplay();

    // 1. Si acercás la mano -> Marcar como vista (✔) y volver al reposo oscuro
    if (isSomeoneNear()) {
      pendingNotif = NOTIF_NONE;
      setMatrixBrightness(10);
      showRows(ICON_CHECK);
      delay(750);
      clearScreen();
      // Esperar un momento a que retires la mano para no disparar "Buen día!" enseguida
      delay(1200);
      pauseStartMs = millis();
      return;
    }

    // 2. Si pasan 3 minutos sin descartarla -> Apagar sola y volver al reposo
    if (millis() - notifStartMs >= NOTIF_TIMEOUT_MS) {
      pendingNotif = NOTIF_NONE;
      clearScreen();
      pauseStartMs = millis();
      return;
    }

    delay(40);
    return;
  }

  // ==========================================================================
  // ESTADO NORMAL DE REPOSO (Pantalla oscura + "Buen día!" + Giro cada 30 s)
  // ==========================================================================
  if (isSomeoneNear()) {
    digitalWrite(SERVO_PIN, LOW);
    randomSeed(random(100000L) ^ micros());
    scrollBuenDia();
    delay(700);
    pauseStartMs = millis();
    return;
  }

  if (millis() - pauseStartMs >= OBSERVE_PAUSE_MS) {
    int randomTargetUs = pickRandomTargetUs();
    bool spottedSomeone = panSmoothTo(randomTargetUs, true);

    if (spottedSomeone && pendingNotif == NOTIF_NONE) {
      scrollBuenDia();
      delay(700);
    }

    pauseStartMs = millis();
  }

  delay(50);
}

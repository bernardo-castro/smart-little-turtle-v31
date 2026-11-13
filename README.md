# Smart Little Turtle V3.1 — Asistente de Escritorio y Notificador Bluetooth BLE

Firmware en **Arduino** y puente de notificaciones en **Python (Windows 11)** para convertir el robot **Keyestudio Smart Little Turtle V3.1** en un compañero silencioso de escritorio que saluda al acercarte y muestra notificaciones en tiempo real de **Gmail** y **Google Chat / Hangouts** vía **Bluetooth Low Energy (DX-BT24)**.

---

## Características Principales

### 1. Cara Viva Permanente Sincronizada 1:1 con el Ángulo del Ultrasonido
- **Ruedas bloqueadas por software (`LOW`)** para uso seguro sobre el escritorio sin desplazarse.
- **Cara sincronizada en tiempo real con el ángulo físico del cuello (brillo `8`):** divide el recorrido del servo ultrasónico en **7 zonas angulares continuas (`-3` a `+3`)**. A medida que el cabezal gira de un lado a otro, la cara en la matriz `8x8` se desplaza paso a paso acompañándolo (cruzando por el centro exacto `0` cuando el ultrasonido pasa por `90°`), y donde el cuello se detenga —izquierda, centro o derecha— **la cara se queda mirando exactamente hacia ese mismo ángulo** mientras pestañea suavemente en 4 niveles (`2.6` a `5.6 s`).
- **Paneo aleatorio con aceleración y frenado suave (`5` a `30 s`):** el servo del cuello (`D10`) arranca y frena progresivamente (*ease-in / ease-out* a `50 Hz`) y corta la señal al terminar para eliminar cualquier zumbido mecánico.
- **Seguimiento vivo y fluido de tu mano (`15..28 cm`) + rabieta si le tapas la vista (`5 s`):** cuando acercas tu mano, engancha tu palma y **sigue tu movimiento suavemente a `50 Hz`** con aceleración/frenado amortiguado (*slew-rate limiter* sub-microsegundo), memoria de dirección y auto-centrado geométrico sobre tu palma/dedos (sin latigazos ni zumbidos), reflejando en la cara cada ángulo del recorrido; si dejas la mano quieta tapándole el frente (`<= 12.5 cm`) durante `5 s` seguidos **se enoja (`>:[`, sacude la cabeza y da un micro-paso atrás)** hasta que la retiras, despidiéndose luego con una sonrisa `^_^`.
- **Reacción al levantarlo de la mesa (`O_o`):** aprovecha los 3 sensores infrarrojos inferiores (`D11, D7, D8`) para detectar al instante si alguien lo alza en el aire, alternando una cara desconcertada **`O_o` / `o_O`** mientras mira a ambos lados, y mostrando una sonrisa **`^_^`** de alivio cuando vuelve a apoyarse sobre el escritorio.

### 2. Cola de Notificaciones Bluetooth BLE (`DX-BT24`) con Agrupación por Remitente
El puente en Windows monitorea en tiempo real las notificaciones emergentes (*toasts*) de Windows 11 (y opcionalmente tu calendario `.ics` en directo), extrae el remitente o la hora de inicio de la reunión y transmite el aviso por **BLE (`9600 bps`)** al robot:

| Evento / Trama BLE | Acción del Robot | Pantalla `8x8` |
| :--- | :--- | :--- |
| **`M:<Remitente>`** (Nuevo correo en Gmail / Outlook) | Encola o agrupa el correo, gira al frente (`90°`), muestra el sobre respirando (`~3.2 s`, brillo `15`) y desplaza el **nombre del remitente** (`Nombre (xN)` si envió varios). | `✉` → `Nombre` |
| **`C:<Remitente>`** (Mensaje en Google Chat / Hangouts o Chat integrado en Gmail) | Encola o agrupa el chat, gira al frente (`90°`), muestra el globo animado (`~3.2 s`, brillo `15`) y desplaza el **nombre de quien escribió** (`Nombre (xN)` si envió varios). | `💬` → `Nombre` |
| **`R:<Min>:<Reunión>`** (Reuniones de Google Calendar, Outlook, Teams, Zoom o `.ics`) | Encola o actualiza la reunión, hace un pequeño giro de atención, muestra un **reloj animado (`⏰`, brillo `15`)** y descuenta los minutos en vivo:<br>• Si falta tiempo (`Min > 0`): **`En 10m: <Reunión>`** (baja minuto a minuto solo)<br>• En el minuto exacto (`Min == 0`): **`Inicia ya: <Reunión>`**<br>• Si ya empezó (`Min < 0`): **`Ya inició (3m): <Reunión>`** (y el reloj gira más rápido) | `⏰` → `En Xm` / `Ya inició` |
| **`B`** (Saludo remoto manual) | Desplaza el mensaje `"Buen día!"` en pantalla. | Texto `Buen día!` |
| **`X`** (Limpiar cola) | Vacía inmediatamente todas las notificaciones pendientes y regresa a la cara viva. | Cara viva |

**Agrupación inteligente, pausa con carita viva y check por persona (`<= 10 cm`):**
- **Avisos breves intercalados con la carita viva:** cada notificación se muestra brevemente (ícono `1.8 s` en brillo `15` + desplazamiento ágil en brillo `8`) y luego **regresa a su carita viva (`16 s`)** antes de volver a recordarte el aviso (o el siguiente de la cola), repitiendo el ciclo sin invadir la pantalla de forma continua. Si llega un mensaje nuevo mientras descansa, lo anuncia de inmediato.
- **Agrupación automática `(xN)`:** si una misma persona envía múltiples mensajes seguidos, se agrupan en un solo ítem mostrando la cantidad acumulada (por ejemplo, `Ana (x4)`), evitando saturar la cola.
- **Check por persona (`✔`):** al acercar la mano a `<= 10 cm` del sensor ultrasónico (tanto durante el aviso como durante la pausa con carita viva), el robot muestra un check **`✔`** y **limpia de una sola vez todas las notificaciones en cola pertenecientes a ese remitente** (incluyendo correos y chats bajo ese mismo nombre), pasando al siguiente remitente o regresando a su estado normal.

### 3. Control Conversacional en Vivo por Bluetooth (`robot_ctl.py` / `/robot`)
Sin interrumpir las notificaciones en segundo plano, `robot_ctl.py` envía órdenes en vivo a través del puente BLE activo (`cmd_inbox.jsonl` → `DX-BT24` → `cmd_reply.json`) o por conexión BLE directa de respaldo:
- **Micro-movimientos seguros sobre escritorio:** `W:F:<ms>`, `W:B:<ms>`, `W:L:<ms>`, `W:R:<ms>`, `W:S` (con freno automático ante bordes por sensores IR inferiores y ante obstáculos frontales `< 11 cm`).
- **Cuello y mirada sincronizada:** `N:L`, `N:C`, `N:R`, `N:NO`, `N:SCAN` o ángulo exacto `N:<40..140>`.
- **Gestos y estados (brillo `8`):** `G:HAPPY`, `G:SURPRISE`, `G:HEART`, `G:THINK`, `G:CELEBRATE`, `G:SLEEP` (`Zzz`) y `G:WAKE`.
- **Texto personalizado y telemetría:** `T:<mensaje>` (con soporte de acentos y `ñ`) y `Q` (`STAT:DIST=...,DESK=...,PULSE=...,ZONE=...,NOTIF=...,SLEEP=...`).

### 4. Inicio Automático Inteligente en Windows (0 Consumo Inactivo)
- Se lanza de forma invisible (`pythonw.exe bt_notifier.py --auto`) **al iniciar sesión en Windows**.
- Si el robot está **apagado o fuera de alcance**, el proceso **se cierra por completo** (`0 MB RAM / 0% CPU`).
- Una **Tarea Programada de Windows** reintenta la búsqueda de forma silenciosa **cada 10 minutos**; si ya está conectado, un `Mutex` de Windows (`Local\TurtleRobotBT24Bridge`) evita instancias duplicadas.

---

## Arquitectura de Hardware y Pinout

- **Placa:** Keyestudio Smart Little Turtle V3.1 (`ATmega328P` / perfil `arduino:avr:uno`, conversor USB-Serie `Silicon Labs CP2102`).
- **Matriz LED Frontal:** `8x8` con controlador **HT16K33** por bus **I2C (`0x70`)** en `A4 (SDA)` y `A5 (SCL)`.
  - Orientación física mapeada en RAM: `RAM[14]` = fila superior (`y=0`) hasta `RAM[0]` = fila inferior (`y=7`); `bit 0` (`0x01`) = columna izquierda (`x=0`) y `bit 7` (`0x80`) = columna derecha (`x=7`).
- **Sensor Ultrasónico (`HC-SR04`):** `TRIG = D12`, `ECHO = D13`.
- **Servo del Cuello (`SG90`):** `D10` (controlado sin `Servo.h` para no interferir con el Timer1 ni con el pin `D9` del puente de motores).
- **Pines de Motores (retenidos en `LOW`):** `D2`, `D4`, `D5`, `D6` y `D9`.
- **Módulo Bluetooth BLE 5.1:** **DX-BT24** conectado al puerto serie por hardware (`D0/RX`, `D1/TX` a `9600 bps`).
  - Servicio GATT BLE: `0000ffe0-0000-1000-8000-00805f9b34fb`
  - Característica GATT UART: `0000ffe1-0000-1000-8000-00805f9b34fb`

> **Nota importante al reprogramar por USB:** El módulo `DX-BT24` comparte las líneas `RX/TX` (`D0/D1`) con el chip USB `CP2102`. Para cargar un nuevo sketch por cable USB, **retira temporalmente el módulo Bluetooth** (con el robot apagado), sube el firmware y vuelve a insertarlo.

---

## Estructura del Repositorio

```text
.
├── turtle_curious/
│   └── turtle_curious.ino                 # Firmware principal para Arduino Uno (ATmega328P)
├── pc_bluetooth_bridge/
│   ├── bt_notifier.py                     # Puente Windows 11 Notification WAL -> BLE DX-BT24 + cola /robot
│   ├── robot_ctl.py                       # CLI para enviar comandos en vivo al robot (/robot)
│   ├── requirements.txt                   # Dependencias de Python (bleak)
│   ├── iniciar_notificaciones.bat         # Lanzador manual interactivo en consola
│   ├── instalar_inicio_automatico.ps1     # Configura arranque con Windows + chequeo cada 10 min
│   └── desinstalar_inicio_automatico.ps1  # Elimina el inicio automático y detiene el proceso
└── driver_cp210x/                         # Driver oficial firmado WHQL Silicon Labs CP210x (USB)
```

---

## Instalación y Uso

### Paso 1: Driver USB CP210x (solo para cargar firmware por cable)
Si Windows no asigna un puerto `COM` al conectar el robot por USB, instala el controlador incluido ejecutando PowerShell **como Administrador**:

```powershell
pnputil /add-driver ".\driver_cp210x\silabser.inf" /install
```

### Paso 2: Compilar y Cargar el Firmware en el Robot
Con el módulo Bluetooth desconectado del zócalo y el cable USB conectado (reemplaza `COM5` por tu puerto si difiere):

```powershell
arduino-cli core install arduino:avr
arduino-cli compile --fqbn arduino:avr:uno .\turtle_curious
arduino-cli upload -p COM5 --fqbn arduino:avr:uno .\turtle_curious
```

Una vez finalizada la carga, apaga el robot, inserta nuevamente el módulo **DX-BT24** y enciéndelo.

### Paso 3: Configurar el Puente Bluetooth en Windows 11

1. **Instalar dependencias de Python (3.10+):**
   ```powershell
   pip install -r .\pc_bluetooth_bridge\requirements.txt
   ```

2. **(Opcional) Configurar la dirección MAC de tu módulo DX-BT24:**
   El script detecta automáticamente módulos con nombre `BT24` o servicio `0xFFE0`, y prioriza la dirección configurada en la variable de entorno `TURTLE_BT24_MAC` (o en la constante `BT24_MAC_ADDRESS` dentro de `bt_notifier.py`).

3. **Habilitar notificaciones de Correo, Chat y Reuniones:**
   - **Gmail y Google Chat:** en *Gmail -> Configuración -> General -> Notificaciones de escritorio*, activa las notificaciones de correos nuevos y de chat.
   - **Reuniones por navegador (Opción A):** abre `https://calendar.google.com` en Chrome, ve a *Configuración -> Configuración de notificaciones -> Notificaciones -> **Notificaciones de escritorio*** y acepta el permiso en Chrome (también detecta avisos de calendario de **Outlook**, **Teams** y **Zoom**).
   - **Reuniones en directo sin tener Chrome abierto (Opción B, recomendada):** copia `pc_bluetooth_bridge\calendar_config.example.json` a `pc_bluetooth_bridge\calendar_config.json` y pega tu **Dirección secreta en formato iCal (`.ics`)** desde *Google Calendar -> Configuración de mi calendario -> Integrar el calendario*. El puente consultará tus reuniones cada minuto en segundo plano.

4. **Activar el arranque con Windows + búsqueda cada 10 minutos:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\pc_bluetooth_bridge\instalar_inicio_automatico.ps1
   ```
   Esto crea:
   - Un lanzador silencioso en `shell:startup` (`TurtleRobotBluetooth.vbs`) para conectar apenas inicia Windows.
   - Una tarea programada (`TurtleRobotBluetooth`) que vuelve a buscar el robot cada **10 minutos** si estaba apagado.

---

## Pruebas Manuales y Diagnóstico

- **Modo consola interactivo:** ejecuta `pc_bluetooth_bridge\iniciar_notificaciones.bat` (debes no tener la instancia automática corriendo o detenerla antes). Permite enviar comandos en vivo presionando:
  - `[m:Juan Perez]` + Enter → Probar aviso de **Correo (`✉`)** con remitente
  - `[c:Ana]` + Enter → Probar aviso de **Chat (`💬`)** con remitente
  - `[r:10:Planificacion]` + Enter → Probar **Reunión (`⏰`)** que inicia en 10 min
  - `[r:0:Daily]` + Enter → Probar **Reunión (`⏰`)** que inicia ahora mismo
  - `[r:-4:Coordinacion]` + Enter → Probar **Reunión (`⏰`)** que ya inició hace 4 min
  - `[b]` + Enter → Disparar saludo **`"Buen día!"`**
  - `[x]` + Enter → Vaciar toda la cola de notificaciones
  - `[q]` + Enter → Salir
- **Registro de actividad:** en modo automático, los eventos de conexión y notificaciones enviadas se guardan en `pc_bluetooth_bridge\bt_notifier.log` (con rotación automática a `256 KB`).
- **Desinstalar el arranque automático:**
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\pc_bluetooth_bridge\desinstalar_inicio_automatico.ps1
  ```

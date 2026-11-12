# Smart Little Turtle V3.1 — Asistente de Escritorio y Notificador Bluetooth BLE

Firmware en **Arduino** y puente de notificaciones en **Python (Windows 11)** para convertir el robot **Keyestudio Smart Little Turtle V3.1** en un compañero silencioso de escritorio que saluda al acercarte y muestra notificaciones en tiempo real de **Gmail** y **Google Chat / Hangouts** vía **Bluetooth Low Energy (DX-BT24)**.

---

## Características Principales

### 1. Modo Observador Silencioso (Estado de Reposo)
- **Ruedas bloqueadas por software (`LOW`)** para uso seguro sobre el escritorio sin desplazarse.
- **Pantalla LED `8x8` apagada** en reposo para no distraer.
- **Paneo aleatorio silencioso:** cada **30 segundos**, el servo del cuello (`D10`) gira suavemente a una posición aleatoria entre `~52°` y `~128°` mediante pulsos manuales a `50 Hz` exactos (`7 µs` por cuadro de `20 ms`), cortando la señal al terminar para eliminar cualquier zumbido mecánico.
- **Saludo por proximidad corta:** cuando el sensor ultrasónico (`HC-SR04`) detecta que pasas la mano bien cerca (`<= 10 cm`), desplaza el texto **`"Buen día!"`** de derecha a izquierda en la matriz `8x8` y vuelve al reposo (evitando activaciones accidentales a media distancia en el escritorio).

### 2. Cola de Notificaciones Bluetooth BLE (`DX-BT24`) con Nombre del Remitente
El puente en Windows monitorea en tiempo real las notificaciones emergentes (*toasts*) de Windows 11, extrae el **nombre de quien envía el mensaje** y transmite el aviso por **BLE (`9600 bps`)** al robot:

| Evento / Trama BLE | Acción del Robot | Pantalla `8x8` |
| :--- | :--- | :--- |
| **`M:<Remitente>`** (Nuevo correo en Gmail / Outlook) | Encola el correo, gira al frente (`90°`), muestra el sobre respirando (`~3.2 s`) y desplaza el **nombre del remitente**. | `✉` → `Nombre` |
| **`C:<Remitente>`** (Mensaje en Google Chat / Hangouts o Chat integrado en Gmail) | Encola el chat, gira al frente (`90°`), muestra el globo animado (`~3.2 s`) y desplaza el **nombre de quien escribió**. | `💬` → `Nombre` |
| **`B`** (Saludo remoto manual) | Desplaza el mensaje `"Buen día!"` en pantalla. | Texto `Buen día!` |
| **`X`** (Limpiar cola) | Vacía inmediatamente todas las notificaciones pendientes y regresa al reposo. | Pantalla apagada |

**Cola inteligente y borrado de a uno con la mano (`<= 10 cm`):**
- **Iteración continua:** si llegan varios mensajes seguidos (hasta `10` en cola), el robot los recorre uno tras otro mostrando su ícono correspondiente (`✉` o `💬`) seguido de su posición y remitente (por ejemplo, `1/3 Sylvia`, `2/3 Renato`, `3/3 Ana`).
- **Borrado individual por proximidad:** cada vez que acercas la mano a `<= 10 cm` del sensor ultrasónico, el robot muestra un tilde **`✔`**, **borra únicamente el mensaje actual** y pasa al siguiente pendiente hasta vaciar la cola y apagar la pantalla.

### 3. Inicio Automático Inteligente en Windows (0 Consumo Inactivo)
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
│   ├── bt_notifier.py                     # Puente Windows 11 Notification WAL -> BLE DX-BT24
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

3. **Habilitar notificaciones de escritorio en Chrome y Gmail:**
   - En **Gmail -> Configuración -> General -> Notificaciones de escritorio**, selecciona *Activar notificaciones de correos electrónicos nuevos*.
   - En **Google Chat** (o Chat dentro de Gmail), activa *Permitir notificaciones de chat*.
   - Verifica que **Google Chrome** tenga permitido mostrar notificaciones en *Configuración de Windows -> Sistema -> Notificaciones*.

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
  - `[c:Sylvia]` + Enter → Probar aviso de **Chat (`💬`)** con remitente
  - `[b]` + Enter → Disparar saludo **`"Buen día!"`**
  - `[x]` + Enter → Vaciar toda la cola de notificaciones
  - `[q]` + Enter → Salir
- **Registro de actividad:** en modo automático, los eventos de conexión y notificaciones enviadas se guardan en `pc_bluetooth_bridge\bt_notifier.log` (con rotación automática a `256 KB`).
- **Desinstalar el arranque automático:**
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\pc_bluetooth_bridge\desinstalar_inicio_automatico.ps1
  ```

"""
Puente Windows 11 -> Bluetooth BLE (DX-BT24) para Smart Little Turtle V3.1
--------------------------------------------------------------------------
Modos de ejecución:
  - Automático silencioso (Inicio de Windows + Tarea cada 10 min):
      pythonw.exe bt_notifier.py --auto
      * Garantiza una única instancia activa mediante Mutex de Windows.
      * Busca tu robot (DX-BT24).
      * Si NO lo encuentra encendido -> SE APAGA POR COMPLETO (0 RAM / 0 CPU)
        hasta que el Programador de Tareas vuelva a probar en 10 minutos.
      * Si lo encuentra -> envía cada notificación con el tipo ('M' o 'C') y
        el nombre del remitente ("C:Sylvia Constanzo\n") para que el robot las
        encole, muestre ícono + nombre y las vaya iterando.

  - Manual interactivo en consola (iniciar_notificaciones.bat):
      python.exe bt_notifier.py
      Comandos:
        m  o  m:Juan Perez   -> Probar aviso de Correo con remitente
        c  o  c:Ana Gomez    -> Probar aviso de Chat con remitente
        b                    -> Saludo "Buen día!"
        x                    -> Limpiar toda la cola
        q                    -> Salir
"""

import asyncio
import ctypes
import html
import os
import re
import sys
import unicodedata
from datetime import datetime
from bleak import BleakClient, BleakScanner

BT24_MAC_ADDRESS = os.environ.get("TURTLE_BT24_MAC", "48:87:2D:6A:E9:6E")
BT24_SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
BT24_CHAR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

MAX_SENDER_CHARS = 22

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(BASE_DIR, "bt_notifier.log")

WPN_WAL_PATH = os.path.expandvars(
    r"%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db-wal"
)

TOAST_BLOCK_RE = re.compile(
    r"<toast\s+([^>]*)>((?:(?!</toast>|<toast\b).)*)</toast>",
    re.DOTALL | re.IGNORECASE,
)
TEXT_TAG_RE = re.compile(r"<text[^>]*>(.*?)</text>", re.DOTALL | re.IGNORECASE)
LAUNCH_ATTR_RE = re.compile(r'launch="([^"]+)"', re.IGNORECASE)
TS_ATTR_RE = re.compile(r'displayTimestamp="([^"]+)"', re.IGNORECASE)

GMAIL_EMBEDDED_CHAT_RE = re.compile(
    r"p#https://mail\.google\.com/#[^\"|]+/\d{10,}", re.IGNORECASE
)

# Mapa de caracteres en español a bytes extendidos de la fuente 8x8 del robot
SPANISH_CHAR_MAP: dict[str, int] = {
    "á": 0x80,
    "à": 0x80,
    "ä": 0x80,
    "é": 0x81,
    "è": 0x81,
    "ë": 0x81,
    "í": 0x82,
    "ì": 0x82,
    "ï": 0x82,
    "ó": 0x83,
    "ò": 0x83,
    "ö": 0x83,
    "ú": 0x84,
    "ù": 0x84,
    "ñ": 0x85,
    "Ñ": 0x86,
    "ü": 0x87,
    "Á": ord("A"),
    "É": ord("E"),
    "Í": ord("I"),
    "Ó": ord("O"),
    "Ú": ord("U"),
    "Ü": ord("U"),
}

_MUTEX_HANDLE = None


def acquire_single_instance_mutex() -> bool:
    global _MUTEX_HANDLE
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.CreateMutexW(None, False, "Local\\TurtleRobotBT24Bridge")
    if kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
        if handle:
            kernel32.CloseHandle(handle)
        return False
    _MUTEX_HANDLE = handle
    return True


def log(msg: str) -> None:
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{now}] {msg}"
    if sys.stdout is not None:
        try:
            print(line, flush=True)
        except Exception:
            pass
    try:
        if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > 262144:
            with open(LOG_FILE, "rb") as rf:
                rf.seek(-100000, os.SEEK_END)
                tail = rf.read()
            with open(LOG_FILE, "wb") as wf:
                wf.write(tail)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def clean_sender_name(raw_sender: str, fallback: str) -> str:
    txt = html.unescape(raw_sender or "").strip()
    # En Google Chat de espacios grupales suele venir "Nombre Apellido (Sala)"
    stripped = re.sub(r"\s*\([^)]*\)\s*$", "", txt).strip()
    if stripped:
        txt = stripped
    # En emails tipo "Nombre <correo@dominio>" quedarse solo con el nombre
    stripped_email = re.sub(r"\s*<[^>]+>\s*$", "", txt).strip().strip('"')
    if stripped_email:
        txt = stripped_email
    txt = re.sub(r"\s+", " ", txt).strip()
    if not txt:
        txt = fallback
    return txt[:MAX_SENDER_CHARS].strip()


def encode_robot_bytes(cmd_prefix: str, sender: str | None = None) -> bytes:
    """Codifica 'C:Sylvia Constanzo\\n' mapeando tildes/ñ a la fuente 8x8 del Arduino."""
    out = bytearray()
    out.extend(cmd_prefix.encode("ascii", errors="ignore"))
    if sender is not None:
        out.append(ord(":"))
        for ch in sender:
            if ch in SPANISH_CHAR_MAP:
                out.append(SPANISH_CHAR_MAP[ch])
            elif 32 <= ord(ch) <= 126:
                out.append(ord(ch))
            else:
                # Intentar quitar diacrítico raro; si es emoji, omitir
                norm = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore")
                if norm:
                    out.extend(norm)
    out.append(ord("\n"))
    return bytes(out)


def classify_toast_launch(launch_str: str) -> str | None:
    low = launch_str.lower()

    if (
        "chat.google.com" in low
        or "hangouts.google.com" in low
        or GMAIL_EMBEDDED_CHAT_RE.search(launch_str)
    ):
        return "C"

    if (
        "mail.google.com" in low
        or "gmail" in low
        or "outlook.office" in low
        or "outlook.live" in low
        or "hxmail" in low
    ):
        return "M"

    return None


class WalToastWatcher:
    def __init__(self) -> None:
        self.last_mtime_ns: int = 0
        self.seen_keys: set[tuple[str, str, str, str]] = set()
        self._prime_existing()

    def _extract_all_toasts(self) -> list[tuple[str, str, str, str]]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            with open(WPN_WAL_PATH, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8", errors="ignore")
        except Exception as exc:
            log(f"Aviso leyendo WAL: {exc}")
            return []

        results: list[tuple[str, str, str, str]] = []
        for attrs, body in TOAST_BLOCK_RE.findall(text):
            lm = LAUNCH_ATTR_RE.search(attrs)
            if not lm:
                continue
            tm = TS_ATTR_RE.search(attrs)
            launch = lm.group(1)
            ts = tm.group(1) if tm else ""
            texts = [
                html.unescape(t).strip()
                for t in TEXT_TAG_RE.findall(body)
            ]
            sender_raw = texts[0] if len(texts) >= 1 else ""
            preview_raw = texts[1] if len(texts) >= 2 else ""
            results.append((launch, ts, sender_raw, preview_raw[:80]))
        return results

    def _prime_existing(self) -> None:
        if not os.path.exists(WPN_WAL_PATH):
            return
        try:
            self.last_mtime_ns = os.stat(WPN_WAL_PATH).st_mtime_ns
            for item in self._extract_all_toasts():
                self.seen_keys.add(item)
        except Exception:
            pass

    def poll_new_notifications(self) -> list[tuple[str, str]]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            st = os.stat(WPN_WAL_PATH)
        except Exception:
            return []

        if st.st_mtime_ns == self.last_mtime_ns:
            return []

        self.last_mtime_ns = st.st_mtime_ns
        events: list[tuple[str, str]] = []

        for item in self._extract_all_toasts():
            if item in self.seen_keys:
                continue
            self.seen_keys.add(item)

            launch, _, sender_raw, _ = item
            kind = classify_toast_launch(launch)
            if not kind:
                continue

            fallback = "Chat" if kind == "C" else "Correo"
            sender = clean_sender_name(sender_raw, fallback)
            events.append((kind, sender))

        return events


async def find_bt24_device():
    for attempt in range(2):
        exact = await BleakScanner.find_device_by_address(BT24_MAC_ADDRESS, timeout=4.0)
        if exact:
            return exact
        if attempt == 0:
            await asyncio.sleep(0.5)

    devices = await BleakScanner.discover(timeout=4.0, return_adv=True)
    for _, (dev, adv) in devices.items():
        name_l = (dev.name or adv.local_name or "").lower()
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        if "bt24" in name_l or BT24_SERVICE_UUID in uuids:
            return dev

    return None


async def send_ble_payload(client: BleakClient, char_uuid: str, payload: bytes) -> None:
    """Envía en fragmentos de <=16 bytes respetando el MTU GATT de 20 bytes del DX-BT24."""
    chunk_size = 16
    for idx in range(0, len(payload), chunk_size):
        part = payload[idx : idx + chunk_size]
        try:
            await client.write_gatt_char(char_uuid, part, response=False)
        except Exception:
            await client.write_gatt_char(char_uuid, part, response=True)
        if idx + chunk_size < len(payload):
            await asyncio.sleep(0.03)


async def console_input_loop(queue: asyncio.Queue) -> None:
    if not sys.stdin or not hasattr(sys.stdin, "isatty") or not sys.stdin.isatty():
        return
    loop = asyncio.get_running_loop()
    while True:
        try:
            line = await loop.run_in_executor(None, sys.stdin.readline)
        except Exception:
            return
        if not line:
            return
        raw = line.strip()
        if not raw:
            continue
        cmd = raw[0].lower()
        if cmd in ("m", "c", "b", "x", "q"):
            await queue.put(raw)


async def main() -> None:
    auto_mode = "--auto" in sys.argv

    if not acquire_single_instance_mutex():
        if not auto_mode:
            log("Ya hay una instancia conectada en segundo plano.")
        return

    if not auto_mode:
        log(
            "Modo interactivo: [m:Nombre]=Mail  [c:Nombre]=Chat  [b]=Buen día  [x]=Limpiar  [q]=Salir"
        )

    cmd_queue: asyncio.Queue[str] = asyncio.Queue()
    if not auto_mode:
        asyncio.create_task(console_input_loop(cmd_queue))

    watcher = WalToastWatcher()

    while True:
        log(f"Buscando robot DX-BT24 ({BT24_MAC_ADDRESS})...")
        dev = await find_bt24_device()

        if not dev:
            if auto_mode:
                log(
                    "Robot no detectado (apagado o fuera de alcance). Cerrando proceso; próximo intento en 10 min."
                )
                return
            log("No veo el DX-BT24 todavía. Reintentando en 5s...")
            await asyncio.sleep(5.0)
            continue

        log(f"Conectando a {dev.name or 'DX-BT24'} [{dev.address}]...")
        try:
            async with BleakClient(dev) as client:
                log("¡CONECTADO por Bluetooth al Turtle Robot! Escuchando Gmail y Chat...")

                target_char = BT24_CHAR_UUID
                for svc in client.services:
                    for ch in svc.characteristics:
                        if ch.uuid.lower() == BT24_CHAR_UUID:
                            target_char = ch.uuid
                            break

                while client.is_connected:
                    while not cmd_queue.empty():
                        raw_manual = await cmd_queue.get()
                        c0 = raw_manual[0].lower()
                        if c0 == "q":
                            log("Cerrando puente Bluetooth.")
                            return
                        if c0 in ("m", "c"):
                            custom_sender = (
                                raw_manual[2:].strip()
                                if len(raw_manual) > 2 and raw_manual[1] == ":"
                                else ("Prueba Mail" if c0 == "m" else "Prueba Chat")
                            )
                            payload = encode_robot_bytes(c0.upper(), custom_sender[:MAX_SENDER_CHARS])
                        else:
                            payload = encode_robot_bytes(c0.upper())
                        await send_ble_payload(client, target_char, payload)
                        log(f"Comando manual enviado: {raw_manual!r}")

                    for kind, sender in watcher.poll_new_notifications():
                        tipo = "GOOGLE CHAT" if kind == "C" else "EMAIL (Gmail)"
                        log(f"¡Notificación de {tipo} de '{sender}'! Enviando al robot...")
                        payload = encode_robot_bytes(kind, sender)
                        await send_ble_payload(client, target_char, payload)
                        await asyncio.sleep(0.08)

                    await asyncio.sleep(0.5)

                log("El robot se desconectó.")
                if auto_mode:
                    await asyncio.sleep(3.0)
                    retry_dev = await BleakScanner.find_device_by_address(
                        BT24_MAC_ADDRESS, timeout=3.0
                    )
                    if not retry_dev:
                        log(
                            "Robot apagado confirmado. Cerrando proceso hasta el próximo chequeo en 10 min."
                        )
                        return

        except Exception as exc:
            log(f"Conexión Bluetooth finalizada ({exc}).")
            if auto_mode:
                log("Cerrando proceso automático hasta el próximo chequeo en 10 min.")
                return
            await asyncio.sleep(4.0)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

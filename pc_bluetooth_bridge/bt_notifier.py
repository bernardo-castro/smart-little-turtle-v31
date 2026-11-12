"""
Puente Windows 11 -> Bluetooth BLE (DX-BT24) para Smart Little Turtle V3.1
--------------------------------------------------------------------------
Modos de ejecución:
  - Automático silencioso (Inicio de Windows + Tarea cada 10 min):
      pythonw.exe bt_notifier.py --auto
      * Garantiza una única instancia activa mediante Mutex de Windows.
      * Busca tu robot (DX-BT24 48:87:2D:6A:E9:6E).
      * Si NO lo encuentra encendido -> SE APAGA POR COMPLETO (0 RAM / 0 CPU)
        hasta que el Programador de Tareas vuelva a probar en 10 minutos.
      * Si lo encuentra -> queda conectado enviando avisos de Gmail y Chat;
        cuando apagues el robot, el programa también se cierra solo.

  - Manual interactivo en consola (iniciar_notificaciones.bat):
      python.exe bt_notifier.py
      Teclas rápidas: [m]=Mail  [c]=Chat  [b]=Buen día  [x]=Limpiar  [q]=Salir
"""

import asyncio
import ctypes
import os
import re
import sys
from datetime import datetime
from bleak import BleakClient, BleakScanner

BT24_MAC_ADDRESS = os.environ.get("TURTLE_BT24_MAC", "48:87:2D:6A:E9:6E")
BT24_SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
BT24_CHAR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(BASE_DIR, "bt_notifier.log")

WPN_WAL_PATH = os.path.expandvars(
    r"%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db-wal"
)

TOAST_TAG_RE = re.compile(
    r'<toast\s+[^>]*launch="([^"]+)"[^>]*displayTimestamp="([^"]+)"[^>]*>',
    re.IGNORECASE,
)

GMAIL_EMBEDDED_CHAT_RE = re.compile(
    r"p#https://mail\.google\.com/#[^\"|]+/\d{10,}", re.IGNORECASE
)

# Mantener referencia global para que el GC no libere el Mutex mientras corre
_MUTEX_HANDLE = None


def acquire_single_instance_mutex() -> bool:
    """Devuelve False si ya hay otra instancia de bt_notifier corriendo."""
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
        # Rotar log simple si supera 256 KB
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
        self.seen_keys: set[tuple[str, str]] = set()
        self._prime_existing()

    def _extract_all_toasts(self) -> list[tuple[str, str]]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            with open(WPN_WAL_PATH, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8", errors="ignore")
            return TOAST_TAG_RE.findall(text)
        except Exception as exc:
            log(f"Aviso leyendo WAL: {exc}")
            return []

    def _prime_existing(self) -> None:
        if not os.path.exists(WPN_WAL_PATH):
            return
        try:
            self.last_mtime_ns = os.stat(WPN_WAL_PATH).st_mtime_ns
            for launch, ts in self._extract_all_toasts():
                self.seen_keys.add((launch, ts))
        except Exception:
            pass

    def poll_new_commands(self) -> list[str]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            st = os.stat(WPN_WAL_PATH)
        except Exception:
            return []

        if st.st_mtime_ns == self.last_mtime_ns:
            return []

        self.last_mtime_ns = st.st_mtime_ns
        cmds: list[str] = []

        for launch, ts in self._extract_all_toasts():
            key = (launch, ts)
            if key in self.seen_keys:
                continue
            self.seen_keys.add(key)

            kind = classify_toast_launch(launch)
            if kind and kind not in cmds:
                cmds.append(kind)

        return cmds


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


async def send_ble_cmd(client: BleakClient, char_uuid: str, cmd: str) -> None:
    data = (cmd.strip() + "\n").encode("ascii")
    try:
        await client.write_gatt_char(char_uuid, data, response=False)
    except Exception:
        await client.write_gatt_char(char_uuid, data, response=True)


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
        cmd = line.strip().lower()
        if cmd in ("m", "c", "b", "x", "q"):
            await queue.put(cmd)


async def main() -> None:
    auto_mode = "--auto" in sys.argv

    if not acquire_single_instance_mutex():
        if not auto_mode:
            log("Ya hay una instancia conectada en segundo plano.")
        return

    if not auto_mode:
        log("Iniciado en modo interactivo. Teclas: [m]=Mail [c]=Chat [b]=Buen día [x]=Limpiar [q]=Salir")

    cmd_queue: asyncio.Queue[str] = asyncio.Queue()
    if not auto_mode:
        asyncio.create_task(console_input_loop(cmd_queue))

    watcher = WalToastWatcher()

    while True:
        log(f"Buscando robot DX-BT24 ({BT24_MAC_ADDRESS})...")
        dev = await find_bt24_device()

        if not dev:
            if auto_mode:
                log("Robot no detectado (apagado o fuera de alcance). Cerrando proceso; próximo intento en 10 min.")
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
                        manual = await cmd_queue.get()
                        if manual == "q":
                            log("Cerrando puente Bluetooth.")
                            return
                        await send_ble_cmd(client, target_char, manual.upper())
                        log(f"Comando manual enviado: '{manual.upper()}'")

                    new_cmds = watcher.poll_new_commands()
                    for nc in new_cmds:
                        tipo = "GOOGLE CHAT / HANGOUTS" if nc == "C" else "EMAIL NUEVO (Gmail)"
                        log(f"¡Notificación Chrome de {tipo}! Enviando '{nc}' al robot...")
                        await send_ble_cmd(client, target_char, nc)

                    await asyncio.sleep(0.5)

                log("El robot se desconectó.")
                if auto_mode:
                    # Dar 1 solo reintento rápido por si fue un microcorte BLE; si se apagó, cerrar
                    await asyncio.sleep(3.0)
                    retry_dev = await BleakScanner.find_device_by_address(BT24_MAC_ADDRESS, timeout=3.0)
                    if not retry_dev:
                        log("Robot apagado confirmado. Cerrando proceso hasta el próximo chequeo en 10 min.")
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

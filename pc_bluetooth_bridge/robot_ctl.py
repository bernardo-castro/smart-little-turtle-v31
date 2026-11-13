"""
CLI de control en vivo para Smart Little Turtle V3.1 (/robot en Antigravity)
----------------------------------------------------------------------------
Envía comandos al robot por Bluetooth BLE (DX-BT24):
  1. Vía puente activo `bt_notifier.py` (`cmd_inbox.jsonl` -> `cmd_reply.json`)
     para compartir la conexión BLE sin perder notificaciones de Gmail/Chat/Calendar.
  2. Fallback directo por `BleakClient` si `bt_notifier.py` no está conectado en ese instante.

Ejemplos:
  python robot_ctl.py Q
  python robot_ctl.py W:F:220
  python robot_ctl.py N:L G:HAPPY "T:Hola Bernardo!"
"""

import asyncio
import json
import os
import sys
import time
import unicodedata
import uuid

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CMD_INBOX_FILE = os.path.join(BASE_DIR, "cmd_inbox.jsonl")
CMD_REPLY_FILE = os.path.join(BASE_DIR, "cmd_reply.json")
HEARTBEAT_FILE = os.path.join(BASE_DIR, "bridge_heartbeat.json")

BT24_MAC_ADDRESS = os.environ.get("TURTLE_BT24_MAC", "48:87:2D:6A:E9:6E")
BT24_SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
BT24_CHAR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

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


def encode_raw_command(cmd_str: str) -> bytes:
    clean = cmd_str.replace("\r", "").replace("\n", "").strip()
    buf = bytearray()
    for ch in clean:
        if ch in SPANISH_CHAR_MAP:
            buf.append(SPANISH_CHAR_MAP[ch])
        elif 32 <= ord(ch) <= 126:
            buf.append(ord(ch))
        else:
            norm = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore")
            if norm:
                buf.extend(norm)
    buf.append(ord("\n"))
    return bytes(buf)


def is_bridge_alive() -> bool:
    if not os.path.exists(HEARTBEAT_FILE):
        return False
    try:
        with open(HEARTBEAT_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return bool(data.get("connected")) and (time.time() - float(data.get("ts", 0.0)) < 6.5)
    except Exception:
        return False


def send_via_bridge(commands: list[str]) -> tuple[bool, list[str]]:
    replies: list[str] = []
    for cmd in commands:
        req_id = uuid.uuid4().hex[:10]
        entry = json.dumps({"id": req_id, "cmd": cmd, "ts": time.time()}, ensure_ascii=False)
        try:
            with open(CMD_INBOX_FILE, "a", encoding="utf-8") as f:
                f.write(entry + "\n")
        except OSError as exc:
            return False, [f"ERR:INBOX:{exc}"]

        deadline = time.time() + 10.5
        got_reply = None
        while time.time() < deadline:
            if os.path.exists(CMD_REPLY_FILE):
                try:
                    with open(CMD_REPLY_FILE, "r", encoding="utf-8") as rf:
                        rep = json.load(rf)
                    if rep.get("id") == req_id:
                        got_reply = str(rep.get("reply", "OK"))
                        break
                except Exception:
                    pass
            time.sleep(0.08)

        if got_reply is None:
            return False, replies + [f"ERR:TIMEOUT ({cmd})"]
        replies.append(got_reply)
        if len(commands) > 1:
            time.sleep(0.15)
    return True, replies


async def send_direct_ble(commands: list[str]) -> tuple[bool, list[str]]:
    try:
        from bleak import BleakClient, BleakScanner
    except Exception as exc:
        return False, [f"ERR:BLEAK_IMPORT:{exc}"]

    dev = await BleakScanner.find_device_by_address(BT24_MAC_ADDRESS, timeout=4.5)
    if not dev:
        devices = await BleakScanner.discover(timeout=4.0, return_adv=True)
        for _, (d, adv) in devices.items():
            name_l = (d.name or adv.local_name or "").lower()
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            if "bt24" in name_l or BT24_SERVICE_UUID in uuids:
                dev = d
                break

    if not dev:
        return False, ["ERR:NO_ROBOT_BLE (DX-BT24 apagado, desconectado o ocupado)"]

    replies: list[str] = []
    async with BleakClient(dev) as client:
        target_char = BT24_CHAR_UUID
        for svc in client.services:
            for ch in svc.characteristics:
                if ch.uuid.lower() == BT24_CHAR_UUID:
                    target_char = ch.uuid
                    break

        ack_queue: asyncio.Queue[str] = asyncio.Queue()
        rx_buf = [""]

        def _on_ack(_, data: bytearray) -> None:
            chunk = data.decode("ascii", errors="ignore")
            if not chunk:
                return
            rx_buf[0] += chunk
            while "\n" in rx_buf[0]:
                line_str, rx_buf[0] = rx_buf[0].split("\n", 1)
                clean_line = line_str.strip()
                if clean_line:
                    try:
                        ack_queue.put_nowait(clean_line)
                    except Exception:
                        pass

        try:
            await client.start_notify(target_char, _on_ack)
        except Exception:
            pass

        for cmd in commands:
            while not ack_queue.empty():
                try:
                    ack_queue.get_nowait()
                except Exception:
                    break

            payload = encode_raw_command(cmd)
            for idx in range(0, len(payload), 16):
                part = payload[idx : idx + 16]
                try:
                    await client.write_gatt_char(target_char, part, response=False)
                except Exception:
                    await client.write_gatt_char(target_char, part, response=True)
                if idx + 16 < len(payload):
                    await asyncio.sleep(0.03)

            try:
                rep = await asyncio.wait_for(ack_queue.get(), timeout=8.5)
            except asyncio.TimeoutError:
                rep = "SENT"
            replies.append(rep)
            if len(commands) > 1:
                await asyncio.sleep(0.2)

    return True, replies


def format_stat_line(line: str) -> str:
    if not line.startswith("STAT:"):
        return line
    raw_kv = line[5:]
    kv: dict[str, str] = {}
    for pair in raw_kv.split(","):
        if "=" in pair:
            k, v = pair.split("=", 1)
            kv[k.strip()] = v.strip()
    dist = kv.get("DIST", "?")
    dist_txt = "despejado (LIBRE)" if dist == "LIBRE" else f"{dist} cm"
    desk = "en escritorio" if kv.get("DESK") == "1" else "levantado en el aire"
    zone_map = {
        "-3": "extremo izq (-3)",
        "-2": "izquierda (-2)",
        "-1": "leve izq (-1)",
        "0": "centro (0)",
        "1": "leve der (+1)",
        "2": "derecha (+2)",
        "3": "extremo der (+3)",
    }
    zone = zone_map.get(kv.get("ZONE", "0"), kv.get("ZONE", "0"))
    notif = kv.get("NOTIF", "0")
    sleeping = "durmiendo (Zzz)" if kv.get("SLEEP") == "1" else "despierto"
    return (
        f"{line}\n"
        f"  -> Estado: {sleeping} | Posición: {desk} | Obstáculo frontal: {dist_txt} | "
        f"Mirada/Cuello: {zone} ({kv.get('PULSE', '?')} us) | Notificaciones pendientes: {notif}"
    )


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    commands = [arg.strip() for arg in sys.argv[1:] if arg.strip()]
    if not commands:
        print("Uso: python robot_ctl.py <COMANDO> [COMANDO_2 ...]")
        return 1

    if is_bridge_alive():
        ok, replies = send_via_bridge(commands)
        if ok:
            for r in replies:
                print(format_stat_line(r))
            return 0

    # Fallback: conexión BLE directa al DX-BT24
    try:
        ok, replies = asyncio.run(send_direct_ble(commands))
    except Exception as exc:
        print(f"ERR:BLE_EXCEPTION:{exc}")
        return 2

    for r in replies:
        print(format_stat_line(r))
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())

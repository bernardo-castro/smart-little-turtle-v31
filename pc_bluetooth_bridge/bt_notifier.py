"""
Puente Windows 11 -> Bluetooth BLE (DX-BT24) para Smart Little Turtle V3.1
--------------------------------------------------------------------------
Soporta:
  - Correos nuevos (Gmail / Outlook)      -> "M:<Remitente>"       (Ícono ✉)
  - Mensajes de Google Chat / Hangouts    -> "C:<Remitente>"       (Ícono 💬)
  - Reuniones (Calendar / Outlook / ICS)  -> "R:<Min>:<Reunión>"   (Ícono ⏰)
      * Min > 0  : "En 10m: <Reunión>" (el robot descuenta cada minuto en vivo)
      * Min == 0 : "Inicia ya: <Reunión>"
      * Min < 0  : "Ya inició (3m): <Reunión>"

Modos de ejecución:
  - Automático silencioso (Inicio de Windows + Tarea cada 10 min):
      pythonw.exe bt_notifier.py --auto
  - Manual interactivo en consola (iniciar_notificaciones.bat):
      python.exe bt_notifier.py
      Comandos de prueba:
        m:Juan Perez         -> Correo nuevo
        c:Sylvia Constanzo   -> Mensaje de Chat
        r:10:Planificacion   -> Reunión que inicia en 10 min
        r:0:Daily TyA        -> Reunión que inicia ahora mismo
        r:-4:Coordinacion    -> Reunión que ya inició hace 4 min
        b                    -> Saludo "Buen día!"
        x                    -> Limpiar toda la cola
        q                    -> Salir
"""

import asyncio
import ctypes
import hashlib
import html
import json
import os
import re
import sys
import unicodedata
import urllib.request
from datetime import date, datetime, timedelta, timezone
from zoneinfo import ZoneInfo
from bleak import BleakClient, BleakScanner

BT24_MAC_ADDRESS = os.environ.get("TURTLE_BT24_MAC", "48:87:2D:6A:E9:6E")
BT24_SERVICE_UUID = "0000ffe0-0000-1000-8000-00805f9b34fb"
BT24_CHAR_UUID = "0000ffe1-0000-1000-8000-00805f9b34fb"

MAX_SENDER_CHARS = 21
RECENT_CATCHUP_SECONDS = 900  # Recuperar avisos de hasta 15 min atrás al reconectar

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(BASE_DIR, "bt_notifier.log")
STATE_FILE = os.path.join(BASE_DIR, "sent_state.json")
CALENDAR_CONFIG_FILE = os.path.join(BASE_DIR, "calendar_config.json")

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

CLOCK_TIME_RE = re.compile(
    r"\b(\d{1,2}):(\d{2})(?:\s*([ap])\.?\s*m\.?)?", re.IGNORECASE
)
REL_FUTURE_MIN_RE = re.compile(
    r"(?:\ben\s+|\bin\s+|\bfaltan\s+)(\d{1,3})\s*(?:min|minutos|minutes|m\b)",
    re.IGNORECASE,
)
REL_PAST_MIN_RE = re.compile(
    r"(?:\bhace\s+|\batrasad[oa]\s+)(\d{1,3})\s*(?:min|minutos|m\b)|(\d{1,3})\s*min(?:utes?)?\s+overdue",
    re.IGNORECASE,
)
NOW_KEYWORDS_RE = re.compile(
    r"\b(ahora|now|en\s+curso|iniciando|starting\s+now|ya\s+inici[oó]|ya\s+comenz[oó]|ha\s+comenzado)\b",
    re.IGNORECASE,
)

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
    if kernel32.GetLastError() == 183:
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


def _encode_robot_str(text: str) -> bytearray:
    buf = bytearray()
    for ch in text:
        if ch in SPANISH_CHAR_MAP:
            buf.append(SPANISH_CHAR_MAP[ch])
        elif 32 <= ord(ch) <= 126:
            buf.append(ord(ch))
        else:
            norm = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore")
            if norm:
                buf.extend(norm)
    return buf


def encode_robot_bytes(
    cmd_prefix: str, label: str | None = None, offset_min: int | None = None
) -> bytes:
    out = bytearray()
    out.extend(cmd_prefix.encode("ascii", errors="ignore"))
    if cmd_prefix.upper() == "R" and label is not None:
        mins = 0 if offset_min is None else max(-999, min(999, int(offset_min)))
        out.extend(f":{mins}:".encode("ascii"))
        out.extend(_encode_robot_str(label.replace(":", " ")))
    elif label is not None:
        out.append(ord(":"))
        out.extend(_encode_robot_str(label))
    out.append(ord("\n"))
    return bytes(out)


def clean_sender_name(raw_sender: str, fallback: str) -> str:
    txt = html.unescape(raw_sender or "").strip()
    stripped = re.sub(r"\s*\([^)]*\)\s*$", "", txt).strip()
    if stripped:
        txt = stripped
    stripped_email = re.sub(r"\s*<[^>]+>\s*$", "", txt).strip().strip('"')
    if stripped_email:
        txt = stripped_email
    txt = re.sub(r"\s+", " ", txt).strip()
    if not txt:
        txt = fallback
    return txt[:MAX_SENDER_CHARS].strip()


def clean_meeting_title(raw_title: str) -> str:
    txt = html.unescape(raw_title or "").strip()
    txt = re.sub(
        r"^(recordatorio|reminder|próxima reunión|reunión|evento)\s*:\s*",
        "",
        txt,
        flags=re.IGNORECASE,
    ).strip()
    txt = re.sub(r"\s+", " ", txt).strip()
    if not txt or txt.lower() in ("calendar.google.com", "google calendar", "calendario"):
        txt = "Reunión"
    return txt[:MAX_SENDER_CHARS].strip()


def _parse_utc_iso(ts_str: str) -> datetime | None:
    if not ts_str:
        return None
    try:
        s = ts_str.strip()
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt
    except Exception:
        return None


def _is_recent_utc_timestamp(ts_str: str, max_age_sec: int) -> bool:
    dt = _parse_utc_iso(ts_str)
    if not dt:
        return False
    age = (datetime.now(timezone.utc) - dt).total_seconds()
    return -60 <= age <= max_age_sec


def parse_meeting_offset_minutes(texts: list[str], ts_str: str, default_min: int = 10) -> int:
    """Calcula cuántos minutos faltan (o negativos si ya inició) respecto al instante actual."""
    joined = " | ".join(texts[1:] if len(texts) > 1 else texts)
    all_joined = " | ".join(texts)

    toast_dt_utc = _parse_utc_iso(ts_str)
    elapsed_since_toast_min = 0
    if toast_dt_utc:
        elapsed_since_toast_min = max(
            0, int(round((datetime.now(timezone.utc) - toast_dt_utc).total_seconds() / 60.0))
        )

    # 1) Buscar hora explícita HH:MM (ej. "09:30 – 10:00" o "9:30 a.m.")
    for candidate_text in (joined, all_joined):
        m_clock = CLOCK_TIME_RE.search(candidate_text)
        if m_clock:
            hh = int(m_clock.group(1))
            mm = int(m_clock.group(2))
            ampm = (m_clock.group(3) or "").lower()
            if ampm == "p" and hh < 12:
                hh += 12
            elif ampm == "a" and hh == 12:
                hh = 0
            if 0 <= hh <= 23 and 0 <= mm <= 59:
                now_local = datetime.now()
                start_local = now_local.replace(hour=hh, minute=mm, second=0, microsecond=0)
                diff_min = int(round((start_local - now_local).total_seconds() / 60.0))
                if -240 <= diff_min <= 720:
                    return diff_min

    # 2) Frases relativas ("en 10 minutos", "hace 5 min", "ahora")
    m_past = REL_PAST_MIN_RE.search(all_joined)
    if m_past:
        mins_ago = int(m_past.group(1) or m_past.group(2))
        return -(mins_ago + elapsed_since_toast_min)

    if NOW_KEYWORDS_RE.search(all_joined):
        return -elapsed_since_toast_min

    m_fut = REL_FUTURE_MIN_RE.search(all_joined)
    if m_fut:
        mins_ahead = int(m_fut.group(1))
        return mins_ahead - elapsed_since_toast_min

    return default_min - elapsed_since_toast_min


def classify_toast_event(
    launch_str: str, ts_str: str, texts: list[str]
) -> tuple[str, str, int | None] | None:
    """Devuelve (kind, label, offset_min) donde kind es 'M', 'C' o 'R'."""
    low = launch_str.lower()
    joined_low = " ".join(texts).lower()

    # 1) Reuniones: Google Calendar, Google Meet, Zoom, Calendario Windows/Outlook/Teams
    is_gcal = "calendar.google.com" in low or "calendar.google.com" in joined_low
    is_meet_call = "meet.google.com" in low or "app.zoom.us" in low
    is_win_cal = "windowslive.calendar" in low
    is_outlook_cal = ("outlook" in low) and bool(
        CLOCK_TIME_RE.search(joined_low)
        or REL_FUTURE_MIN_RE.search(joined_low)
        or REL_PAST_MIN_RE.search(joined_low)
        or "recordatorio" in joined_low
        or "reminder" in joined_low
    )
    is_teams_meet = ("msteams" in low) and any(
        w in joined_low
        for w in ("reunión", "reunion", "meeting", "comenzó", "started", "unirse", "join")
    )

    if is_gcal or is_meet_call or is_win_cal or is_outlook_cal or is_teams_meet:
        raw_title = texts[0] if texts else "Reunión"
        title = clean_meeting_title(raw_title)
        default_offset = 0 if (is_meet_call or is_teams_meet) else 10
        offset_min = parse_meeting_offset_minutes(texts, ts_str, default_min=default_offset)
        return ("R", title, offset_min)

    # 2) Google Chat / Hangouts (incluyendo Chat embebido en Gmail)
    if (
        "chat.google.com" in low
        or "hangouts.google.com" in low
        or GMAIL_EMBEDDED_CHAT_RE.search(launch_str)
    ):
        raw_sender = texts[0] if texts else ""
        return ("C", clean_sender_name(raw_sender, "Chat"), None)

    # 3) Correo electrónico (Gmail / Outlook)
    if (
        "mail.google.com" in low
        or "gmail" in low
        or "outlook.office" in low
        or "outlook.live" in low
        or "hxmail" in low
    ):
        raw_sender = texts[0] if texts else ""
        return ("M", clean_sender_name(raw_sender, "Correo"), None)

    return None


def _hash_str(raw: str) -> str:
    return hashlib.sha256(raw.encode("utf-8", errors="ignore")).hexdigest()[:24]


class StateStore:
    def __init__(self) -> None:
        self.seen_hashes: list[str] = []
        self.seen_set: set[str] = set()
        self.existed_on_disk = os.path.exists(STATE_FILE)
        if self.existed_on_disk:
            try:
                with open(STATE_FILE, "r", encoding="utf-8") as f:
                    data = json.load(f)
                for h in data.get("seen", []):
                    self.add(str(h), save=False)
            except Exception:
                pass

    def has(self, h: str) -> bool:
        return h in self.seen_set

    def add(self, h: str, save: bool = True) -> None:
        if h not in self.seen_set:
            self.seen_set.add(h)
            self.seen_hashes.append(h)
            if len(self.seen_hashes) > 500:
                dropped = self.seen_hashes[:-350]
                self.seen_hashes = self.seen_hashes[-350:]
                for d in dropped:
                    self.seen_set.discard(d)
            if save:
                self.save()

    def save(self) -> None:
        try:
            with open(STATE_FILE, "w", encoding="utf-8") as f:
                json.dump({"seen": self.seen_hashes}, f)
        except Exception:
            pass


class WalToastWatcher:
    def __init__(self, store: StateStore) -> None:
        self.store = store
        self.last_mtime_ns: int = 0
        self._init_from_wal()

    def _extract_all_toasts(self) -> list[tuple[str, str, list[str]]]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            with open(WPN_WAL_PATH, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8", errors="ignore")
        except Exception as exc:
            log(f"Aviso leyendo WAL: {exc}")
            return []

        results: list[tuple[str, str, list[str]]] = []
        for attrs, body in TOAST_BLOCK_RE.findall(text):
            lm = LAUNCH_ATTR_RE.search(attrs)
            if not lm:
                continue
            tm = TS_ATTR_RE.search(attrs)
            launch = lm.group(1)
            ts = tm.group(1) if tm else ""
            texts = [html.unescape(t).strip() for t in TEXT_TAG_RE.findall(body)]
            results.append((launch, ts, texts))
        return results

    @staticmethod
    def _item_hash(launch: str, ts: str, texts: list[str]) -> str:
        t0 = texts[0] if len(texts) >= 1 else ""
        t1 = texts[1][:80] if len(texts) >= 2 else ""
        return _hash_str(f"wal\x1f{launch}\x1f{ts}\x1f{t0}\x1f{t1}")

    def _init_from_wal(self) -> None:
        changed = False
        for launch, ts, texts in self._extract_all_toasts():
            h = self._item_hash(launch, ts, texts)
            if not self.store.has(h) and not _is_recent_utc_timestamp(
                ts, RECENT_CATCHUP_SECONDS
            ):
                self.store.add(h, save=False)
                changed = True
        if changed or not self.store.existed_on_disk:
            self.store.save()

    def poll_new_events(
        self, force_scan: bool = False
    ) -> list[tuple[str, str, int | None]]:
        if not os.path.exists(WPN_WAL_PATH):
            return []
        try:
            st = os.stat(WPN_WAL_PATH)
        except Exception:
            return []

        if not force_scan and st.st_mtime_ns == self.last_mtime_ns:
            return []

        self.last_mtime_ns = st.st_mtime_ns
        events: list[tuple[str, str, int | None]] = []
        changed = False

        for launch, ts, texts in self._extract_all_toasts():
            h = self._item_hash(launch, ts, texts)
            if self.store.has(h):
                continue
            self.store.add(h, save=False)
            changed = True

            if force_scan and ts and not _is_recent_utc_timestamp(ts, RECENT_CATCHUP_SECONDS):
                continue

            parsed = classify_toast_event(launch, ts, texts)
            if parsed:
                events.append(parsed)

        if changed:
            self.store.save()

        return events


# ============================================================================
# Lector opcional directo de calendarios .ics (Google Calendar / Outlook ICS)
# ============================================================================
WEEKDAY_MAP = {"MO": 0, "TU": 1, "WE": 2, "TH": 3, "FR": 4, "SA": 5, "SU": 6}


def _parse_ics_datetime(prop_params: str, val: str) -> datetime | None:
    val = val.strip()
    if len(val) == 8 and val.isdigit():
        return None  # Evento de día completo sin hora puntual
    try:
        if val.endswith("Z"):
            dt_utc = datetime.strptime(val, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
            return dt_utc.astimezone()
        dt_naive = datetime.strptime(val[:15], "%Y%m%dT%H%M%S")
        m_tz = re.search(r"TZID=([^;:]+)", prop_params, re.IGNORECASE)
        if m_tz:
            try:
                tz = ZoneInfo(m_tz.group(1).strip('"'))
                return dt_naive.replace(tzinfo=tz).astimezone()
            except Exception:
                pass
        return dt_naive.astimezone()
    except Exception:
        return None


def _unescape_ics_text(val: str) -> str:
    return (
        val.replace("\\n", " ")
        .replace("\\N", " ")
        .replace("\\,", ",")
        .replace("\\;", ";")
        .replace("\\\\", "\\")
        .strip()
    )


def _occurrence_today(
    dtstart: datetime, rrule: str, exdates: set[date], today: date
) -> datetime | None:
    if today in exdates:
        return None
    if dtstart.date() == today:
        return dtstart
    if not rrule or dtstart.date() > today:
        return None

    parts = dict(
        p.split("=", 1) for p in rrule.upper().split(";") if "=" in p
    )
    freq = parts.get("FREQ", "")
    until_raw = parts.get("UNTIL", "")
    if until_raw:
        until_dt = _parse_ics_datetime("", until_raw)
        if until_dt and until_dt.date() < today:
            return None
        if len(until_raw) >= 8 and until_raw[:8].isdigit():
            u_date = datetime.strptime(until_raw[:8], "%Y%m%d").date()
            if u_date < today:
                return None

    interval = int(parts.get("INTERVAL", "1") or "1")
    candidate = dtstart.replace(year=today.year, month=today.month, day=today.day)

    if freq == "DAILY":
        days_diff = (today - dtstart.date()).days
        if days_diff >= 0 and days_diff % interval == 0:
            return candidate
    elif freq == "WEEKLY":
        byday = parts.get("BYDAY", "")
        allowed_days = (
            {WEEKDAY_MAP[d[-2:]] for d in byday.split(",") if d[-2:] in WEEKDAY_MAP}
            if byday
            else {dtstart.weekday()}
        )
        if today.weekday() in allowed_days:
            weeks_diff = ((today - timedelta(days=today.weekday())) - (dtstart.date() - timedelta(days=dtstart.weekday()))).days // 7
            if weeks_diff >= 0 and weeks_diff % interval == 0:
                return candidate
    return None


class IcsCalendarPoller:
    def __init__(self, store: StateStore) -> None:
        self.store = store
        self.last_poll_ts: float = 0.0

    def _load_config(self) -> tuple[list[str], int]:
        if not os.path.exists(CALENDAR_CONFIG_FILE):
            return ([], 10)
        try:
            with open(CALENDAR_CONFIG_FILE, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            urls = [str(u).strip() for u in cfg.get("ics_urls", []) if str(u).strip()]
            mins = int(cfg.get("remind_minutes_before", 10))
            return (urls, max(1, min(120, mins)))
        except Exception as exc:
            log(f"Aviso leyendo calendar_config.json: {exc}")
            return ([], 10)

    def poll_due_meetings(self) -> list[tuple[str, str, int]]:
        now_mono = asyncio.get_event_loop().time()
        if self.last_poll_ts and (now_mono - self.last_poll_ts < 60.0):
            return []
        self.last_poll_ts = now_mono

        urls, remind_before = self._load_config()
        if not urls:
            return []

        now_local = datetime.now().astimezone()
        today = now_local.date()
        due_events: list[tuple[str, str, int]] = []

        for url in urls:
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "TurtleRobotBridge/1.0"})
                with urllib.request.urlopen(req, timeout=8) as resp:
                    raw_ics = resp.read().decode("utf-8", errors="ignore")
            except Exception as exc:
                log(f"Aviso consultando calendario ICS: {exc}")
                continue

            unfolded = re.sub(r"\r?\n[ \t]", "", raw_ics)
            for block in unfolded.split("BEGIN:VEVENT")[1:]:
                vevent = block.split("END:VEVENT")[0]
                uid = ""
                summary = "Reunión"
                status = ""
                dtstart: datetime | None = None
                rrule = ""
                exdates: set[date] = set()

                for line in vevent.splitlines():
                    if ":" not in line:
                        continue
                    left, val = line.split(":", 1)
                    prop_parts = left.split(";", 1)
                    prop = prop_parts[0].upper().strip()
                    params = prop_parts[1] if len(prop_parts) > 1 else ""

                    if prop == "UID":
                        uid = val.strip()
                    elif prop == "SUMMARY":
                        summary = clean_meeting_title(_unescape_ics_text(val))
                    elif prop == "STATUS":
                        status = val.strip().upper()
                    elif prop == "DTSTART":
                        dtstart = _parse_ics_datetime(params, val)
                    elif prop == "RRULE":
                        rrule = val.strip()
                    elif prop == "EXDATE":
                        for item in val.split(","):
                            ex_dt = _parse_ics_datetime(params, item)
                            if ex_dt:
                                exdates.add(ex_dt.date())

                if status == "CANCELLED" or not dtstart:
                    continue

                occ = _occurrence_today(dtstart, rrule, exdates, today)
                if not occ:
                    continue

                diff_min = int(round((occ - now_local).total_seconds() / 60.0))
                # Avisar desde `remind_before` min antes hasta 15 min después de iniciada
                if -15 <= diff_min <= remind_before:
                    stage = "start" if diff_min <= 0 else "pre"
                    key_hash = _hash_str(f"ics\x1f{uid or summary}\x1f{occ.isoformat()}\x1f{stage}")
                    if not self.store.has(key_hash):
                        self.store.add(key_hash)
                        due_events.append(("R", summary, diff_min))

        return due_events


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
        if cmd in ("m", "c", "r", "k", "b", "x", "q"):
            await queue.put(raw)


async def main() -> None:
    auto_mode = "--auto" in sys.argv

    if not acquire_single_instance_mutex():
        if not auto_mode:
            log("Ya hay una instancia conectada en segundo plano.")
        return

    if not auto_mode:
        log(
            "Modo interactivo: [m:Nombre]=Mail  [c:Nombre]=Chat  [r:10:Titulo]=Reunión  [b]=Buen día  [x]=Limpiar  [q]=Salir"
        )

    cmd_queue: asyncio.Queue[str] = asyncio.Queue()
    if not auto_mode:
        asyncio.create_task(console_input_loop(cmd_queue))

    store = StateStore()
    watcher = WalToastWatcher(store)
    ics_poller = IcsCalendarPoller(store)

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
                log("¡CONECTADO por Bluetooth al Turtle Robot! Escuchando Mail, Chat y Reuniones...")

                target_char = BT24_CHAR_UUID
                for svc in client.services:
                    for ch in svc.characteristics:
                        if ch.uuid.lower() == BT24_CHAR_UUID:
                            target_char = ch.uuid
                            break

                def _on_robot_ack(_, data: bytearray) -> None:
                    txt = data.decode("ascii", errors="ignore").strip()
                    if txt:
                        log(f"[ACK Robot] {txt}")

                try:
                    await client.start_notify(target_char, _on_robot_ack)
                except Exception:
                    pass

                first_cycle = True
                while client.is_connected:
                    while not cmd_queue.empty():
                        raw_manual = await cmd_queue.get()
                        c0 = raw_manual[0].lower()
                        if c0 == "q":
                            log("Cerrando puente Bluetooth.")
                            return
                        if c0 in ("r", "k"):
                            # Acepta "r:10:Planificacion" o "r"
                            parts = raw_manual.split(":", 2)
                            mins_val = int(parts[1]) if len(parts) >= 2 and parts[1].lstrip("-").isdigit() else 5
                            m_title = parts[2].strip() if len(parts) >= 3 and parts[2].strip() else "Demo Reunion"
                            payload = encode_robot_bytes("R", m_title[:MAX_SENDER_CHARS], mins_val)
                        elif c0 in ("m", "c"):
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

                    pending = watcher.poll_new_events(force_scan=first_cycle)
                    pending.extend(ics_poller.poll_due_meetings())

                    for kind, label, offset_min in pending:
                        if kind == "R":
                            estado = (
                                f"en {offset_min}m"
                                if (offset_min or 0) > 0
                                else ("inicia ya" if (offset_min or 0) == 0 else f"ya inició ({-(offset_min or 0)}m)")
                            )
                            log(f"¡Aviso de REUNIÓN '{label}' ({estado})! Enviando al robot...")
                            payload = encode_robot_bytes("R", label, offset_min)
                        else:
                            tipo = "GOOGLE CHAT" if kind == "C" else "EMAIL (Gmail)"
                            log(f"¡Notificación de {tipo} de '{label}'! Enviando al robot...")
                            payload = encode_robot_bytes(kind, label)
                        await send_ble_payload(client, target_char, payload)
                        await asyncio.sleep(0.15)

                    first_cycle = False
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

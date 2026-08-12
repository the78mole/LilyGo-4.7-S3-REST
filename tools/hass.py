"""
Minimal Home Assistant REST client for the dashboard data sources.

Credentials come from the repo-root .env (gitignored):

    HOMEASSISTANT_URL=http://<host>:8123
    HOMEASSISTANT_TOKEN=<long-lived access token>

Only two services are used:

  * tibber.get_prices        -> quarter-hourly electricity prices
  * weather.get_forecasts    -> hourly/daily forecast for one weather entity

Both are "response services", so they need ?return_response on the REST call.
"""

from __future__ import annotations

import datetime as _dt
import os
from pathlib import Path

import requests

DEFAULT_TIMEOUT = 30


def load_env(env_path: Path | None = None) -> dict[str, str]:
    """Reads the repo-root .env without adding a dependency on python-dotenv."""
    if env_path is None:
        env_path = Path(__file__).resolve().parent.parent / ".env"
    values: dict[str, str] = {}
    if env_path.exists():
        for line in env_path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            values[k.strip()] = v.strip().strip('"').strip("'")
    # Real environment wins, so CI/one-off overrides work.
    values.update({k: v for k, v in os.environ.items() if k.startswith("HOMEASSISTANT_")})
    return values


class HassError(RuntimeError):
    pass


class Hass:
    def __init__(self, url: str | None = None, token: str | None = None):
        env = load_env()
        self.url = (url or env.get("HOMEASSISTANT_URL", "")).rstrip("/")
        self.token = token or env.get("HOMEASSISTANT_TOKEN", "")
        if not self.url or not self.token:
            raise HassError(
                "HOMEASSISTANT_URL / HOMEASSISTANT_TOKEN missing -- set them in .env"
            )
        self.session = requests.Session()
        self.session.headers.update({"Authorization": f"Bearer {self.token}"})

    def call_service(self, domain: str, service: str, data: dict) -> dict:
        resp = self.session.post(
            f"{self.url}/api/services/{domain}/{service}",
            params={"return_response": ""},
            json=data,
            timeout=DEFAULT_TIMEOUT,
        )
        if not resp.ok:
            raise HassError(f"{domain}.{service} -> HTTP {resp.status_code}: {resp.text[:200]}")
        return resp.json().get("service_response", {})

    # ---------------------------------------------------------------- tibber

    def tibber_prices(self, days: int = 2) -> dict[_dt.date, list[float]]:
        """Returns {date: [ct/kWh, ...]} with one entry per quarter hour.

        Tibber reports EUR/kWh; the panel's chart axis is in ct/kWh, so values
        are scaled by 100 here rather than on the device."""
        today = _dt.date.today()
        start = _dt.datetime.combine(today, _dt.time.min)
        end = start + _dt.timedelta(days=days)

        resp = self.call_service(
            "tibber",
            "get_prices",
            {
                "start": start.strftime("%Y-%m-%d %H:%M:%S"),
                "end": end.strftime("%Y-%m-%d %H:%M:%S"),
            },
        )
        homes = resp.get("prices", {})
        if not homes:
            raise HassError("tibber.get_prices returned no homes")

        entries = next(iter(homes.values()))
        by_day: dict[_dt.date, list[float]] = {}
        for e in entries:
            ts = _dt.datetime.fromisoformat(e["start_time"])
            by_day.setdefault(ts.date(), []).append(round(e["price"] * 100.0, 2))
        return by_day

    # --------------------------------------------------------------- weather

    def weather_forecast(self, entity_id: str, kind: str = "hourly") -> list[dict]:
        resp = self.call_service(
            "weather", "get_forecasts", {"type": kind, "entity_id": entity_id}
        )
        ent = resp.get(entity_id)
        if not ent:
            raise HassError(f"no forecast returned for {entity_id}")
        return ent.get("forecast", [])

    def weather_state(self, entity_id: str) -> dict:
        resp = self.session.get(f"{self.url}/api/states/{entity_id}", timeout=DEFAULT_TIMEOUT)
        if not resp.ok:
            raise HassError(f"state {entity_id} -> HTTP {resp.status_code}")
        return resp.json()


def build_weather_payload(hass: Hass, entity_id: str, slot: str = "top-right") -> dict:
    """Condenses HA's hourly forecast into what the panel widget needs."""
    state = hass.weather_state(entity_id)
    attrs = state.get("attributes", {})
    hourly = hass.weather_forecast(entity_id, "hourly")

    now = _dt.datetime.now(_dt.timezone.utc)
    upcoming = []
    for f in hourly:
        ts = _dt.datetime.fromisoformat(f["datetime"])
        if ts >= now:
            upcoming.append((ts, f))

    # Min/max across the rest of today (local time), falling back to the next
    # 24 entries if today is nearly over.
    local_today = _dt.datetime.now().date()
    today_temps = [
        f["temperature"]
        for ts, f in upcoming
        if ts.astimezone().date() == local_today and "temperature" in f
    ]
    if len(today_temps) < 2:
        today_temps = [f["temperature"] for _, f in upcoming[:24] if "temperature" in f]

    next24 = upcoming[:24]

    payload = {
        "slot": slot,
        "title": f"Wetter - {attrs.get('friendly_name', entity_id)}",
        "condition": state.get("state", "cloudy"),
        "temp": attrs.get("temperature", 0.0),
        "precip": sum(f.get("precipitation", 0.0) or 0.0 for _, f in next24),
        "wind": attrs.get("wind_speed", 0.0),
    }
    if today_temps:
        payload["temp_min"] = min(today_temps)
        payload["temp_max"] = max(today_temps)

    # Sparkline series for the next 24 hours. Not every weather integration
    # supplies precipitation_probability (weather.homebw does not,
    # weather.openweathermap does), so send it only when it is actually there
    # rather than fabricating zeros.
    temps = [f.get("temperature") for _, f in next24]
    if all(t is not None for t in temps) and len(temps) >= 2:
        payload["temp_series"] = [round(float(t), 1) for t in temps]

    pops = [f.get("precipitation_probability") for _, f in next24]
    if any(p is not None for p in pops):
        payload["pop_series"] = [round(float(p or 0.0), 0) for p in pops]
        payload["precip_prob"] = max(float(p or 0.0) for p in pops)

    # Four evenly spaced look-ahead slots (+3h, +6h, +9h, +12h).
    fc = []
    for offset in (3, 6, 9, 12):
        if offset - 1 < len(upcoming):
            ts, f = upcoming[offset - 1]
            fc.append({"label": ts.astimezone().strftime("%Hh"),
                       "temp": f.get("temperature", 0.0)})
    if fc:
        payload["forecast"] = fc
    return payload


# --------------------------------------------------------------- agenda ---

# Origin abbreviations shown in front of every entry. Kept explicit rather
# than derived, because auto-abbreviating entity ids produces collisions
# (calendar.elektro_glaser and todo.elektro_glaser both want "El").
SRC_ABBREV = {
    "calendar.daniel": "Da",
    "calendar.elektro_glaser": "EG",
    "calendar.glasers": "Gl",
    "calendar.geburtstage_2": "GB",
    "todo.bizzmark": "Bz",
    "todo.elektro_glaser": "EG",
    "todo.ewerh": "Ew",
    "todo.liste_von_dgbeeco": "dg",
    "todo.meine_aufgaben": "MA",
    "todo.alte_erinnerungen_von_google_notizen": "AE",
}

AGENDA_CALENDARS = [
    "calendar.daniel",
    "calendar.elektro_glaser",
    "calendar.glasers",
    "calendar.geburtstage_2",
]

AGENDA_TODO_LISTS = [
    "todo.bizzmark",
    "todo.elektro_glaser",
    "todo.ewerh",
    "todo.liste_von_dgbeeco",
    "todo.meine_aufgaben",
    "todo.alte_erinnerungen_von_google_notizen",
]

WASTE_CALENDAR = "calendar.birkenweg_erlangen_bruck_mein_abfallkalender"

# Summary text -> single letter. Matched case-insensitively on a substring,
# since the calendar's summaries carry trailing whitespace.
WASTE_LETTERS = [
    ("restm", "R"),
    ("gelber", "G"),
    ("bio", "B"),
    ("papier", "P"),
]

_WEEKDAYS = ("Mo", "Di", "Mi", "Do", "Fr", "Sa", "So")


def _abbrev(entity_id: str) -> str:
    if entity_id in SRC_ABBREV:
        return SRC_ABBREV[entity_id]
    name = entity_id.split(".", 1)[-1]
    return (name[:2].capitalize()) if name else "?"


def calendar_events(hass: Hass, entity_id: str, start, end) -> list[dict]:
    resp = hass.session.get(
        f"{hass.url}/api/calendars/{entity_id}",
        params={"start": start.isoformat(), "end": end.isoformat()},
        timeout=DEFAULT_TIMEOUT,
    )
    if not resp.ok:
        raise HassError(f"calendar {entity_id} -> HTTP {resp.status_code}")
    return resp.json()


def _event_start(ev: dict):
    """Returns (datetime, all_day). All-day entries carry 'date', timed ones
    'dateTime'; both are normalised to an aware datetime for sorting."""
    s = ev.get("start", {})
    if "dateTime" in s:
        return _dt.datetime.fromisoformat(s["dateTime"]), False
    d = _dt.date.fromisoformat(s["date"])
    return _dt.datetime.combine(d, _dt.time.min).astimezone(), True


def build_agenda_payload(hass: Hass, slot: str = "top-left", hours: int = 48) -> dict:
    now = _dt.datetime.now().astimezone()
    end = now + _dt.timedelta(hours=hours)

    # --- appointments, merged across calendars and sorted by start ---
    merged = []
    for cal in AGENDA_CALENDARS:
        try:
            for ev in calendar_events(hass, cal, now, end):
                ts, all_day = _event_start(ev)
                merged.append((ts, all_day, cal, ev))
        except HassError:
            # One unreachable calendar must not blank the whole cell.
            continue
    merged.sort(key=lambda t: t[0])

    events = []
    for ts, all_day, cal, ev in merged[:6]:
        day = _WEEKDAYS[ts.weekday()]
        when = day if all_day else f"{day} {ts:%H:%M}"
        events.append({
            "when": when,
            "text": (ev.get("summary") or "").strip(),
            "src": _abbrev(cal),
        })

    # --- open tasks ---
    todos = []
    for lst in AGENDA_TODO_LISTS:
        try:
            resp = hass.call_service("todo", "get_items",
                                     {"entity_id": lst, "status": "needs_action"})
        except HassError:
            continue
        for item in resp.get(lst, {}).get("items", []):
            todos.append({"text": (item.get("summary") or "").strip(),
                          "src": _abbrev(lst)})
    todos = todos[:6]

    # --- waste collection ---
    waste = []
    try:
        upcoming = calendar_events(hass, WASTE_CALENDAR, now,
                                   now + _dt.timedelta(days=28))
    except HassError:
        upcoming = []
    for ev in sorted(upcoming, key=lambda e: _event_start(e)[0])[:8]:
        summary = (ev.get("summary") or "").strip().lower()
        letter = next((l for key, l in WASTE_LETTERS if key in summary), None)
        if letter is None:
            continue
        ts, _ = _event_start(ev)
        waste.append({"letter": letter,
                      "label": f"{_WEEKDAYS[ts.weekday()]} {ts.day}."})
        if len(waste) >= 4:
            break

    return {"slot": slot, "title": "Termine & Aufgaben",
            "events": events, "todos": todos, "waste": waste}

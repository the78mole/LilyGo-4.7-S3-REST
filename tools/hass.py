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

    payload = {
        "slot": slot,
        "title": f"Wetter - {attrs.get('friendly_name', entity_id)}",
        "condition": state.get("state", "cloudy"),
        "temp": attrs.get("temperature", 0.0),
        "precip": sum(f.get("precipitation", 0.0) or 0.0 for _, f in upcoming[:24]),
        "wind": attrs.get("wind_speed", 0.0),
    }
    if today_temps:
        payload["temp_min"] = min(today_temps)
        payload["temp_max"] = max(today_temps)

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

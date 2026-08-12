#!/usr/bin/env python3
"""
REST client for the LilyGo T5-4.7 S3 e-paper firmware.

Run via `uv run epd_client.py <command> ...` -- uv resolves dependencies
from pyproject.toml in this directory automatically, no manual venv needed.

Commands:
    upload FILE --filename NAME     Stream FILE to the device's SD card.
    convert IMAGE OUT.bin           Convert a PNG/JPG/etc. to the device's
                                     raw grayscale format (see app_config.h
                                     on the firmware side for the format).
    display-image --filename NAME --x X --y Y
                                     Draw a previously uploaded raw image.
    display-text --text TEXT --x X --y Y [--size N]
                                     Draw a text label.
    upload-demo                     Generate + upload a demo image and a
                                     text label, then place both. Used by
                                     `make upload-assets`.
    dashboard                       Render the 2x2 pseudo dashboard
                                     (tasks / weather / power prices) and
                                     push it as one full-screen image.
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path

import requests
from PIL import Image, ImageDraw

DEFAULT_PORT = 80
DEFAULT_TIMEOUT = 30


class EpdClientError(RuntimeError):
    pass


class EpdClient:
    def __init__(self, host: str, port: int = DEFAULT_PORT):
        self.base_url = f"http://{host}:{port}"
        self.session = requests.Session()

    def _post(self, path: str, **kwargs) -> requests.Response:
        resp = self.session.post(f"{self.base_url}{path}", timeout=DEFAULT_TIMEOUT, **kwargs)
        if not resp.ok:
            raise EpdClientError(f"POST {path} -> HTTP {resp.status_code}: {resp.text}")
        return resp

    def upload(self, filename: str, data: bytes | io.BufferedIOBase) -> None:
        """Streams `data` to <sdcard>/<filename> without loading it fully
        into memory on either end -- `requests` streams file-like objects
        chunk by chunk, and the firmware writes each chunk straight to
        the SD card as it arrives."""
        self._post("/api/upload", params={"filename": filename}, data=data)

    def display_image(self, filename: str, x: int, y: int) -> None:
        self._post("/api/display/image", json={"filename": filename, "x": x, "y": y})

    def display_text(self, text: str, x: int, y: int, size: int = 18) -> None:
        self._post("/api/display/text", json={"text": text, "x": x, "y": y, "size": size})

    def display_agenda(self, payload: dict) -> None:
        self._post("/api/display/agenda", json=payload)

    def display_list(self, items, slot: str = "top-left", title: str = "Aufgaben") -> None:
        self._post("/api/display/list",
                   json={"items": items, "slot": slot, "title": title})

    def display_weather(self, payload: dict) -> None:
        self._post("/api/display/weather", json=payload)

    def display_chart_empty(self, slot: str = "tomorrow", **overrides) -> None:
        """Renders the framed 'no data yet' card. Used for tomorrow before the
        day-ahead prices are published, so the cell states why it is blank
        instead of silently keeping whatever was on screen before."""
        payload = {"values": [], "slot": slot}
        payload.update(overrides)
        self._post("/api/display/chart", json=payload)

    def display_chart(self, values, slot: str = "today", **overrides) -> None:
        """Pushes data points only -- the device supplies the fixed 0..60
        ct/kWh axis, the 15-minute slot width, the cell geometry, and (for
        `slot="today"`) derives the highlighted quarter-hour from its own
        NTP-synced clock."""
        payload = {"values": [round(float(v), 2) for v in values], "slot": slot}
        payload.update(overrides)
        self._post("/api/display/chart", json=payload)


def encode_raw_image(img: Image.Image) -> bytes:
    """Packs a PIL image into the firmware's raw format: u16 width, u16
    height (little-endian), followed by 8-bit grayscale pixels, row-major."""
    gray = img.convert("L")
    width, height = gray.size
    if width > 0xFFFF or height > 0xFFFF:
        raise EpdClientError(f"image too large for u16 header: {width}x{height}")
    header = struct.pack("<HH", width, height)
    return header + gray.tobytes()


def make_demo_image(width: int = 400, height: int = 200) -> bytes:
    """Procedurally generates a small test card (gradient + border + text)
    so `make upload-assets` works without any bundled binary assets."""
    img = Image.new("L", (width, height), color=255)
    draw = ImageDraw.Draw(img)

    for x in range(width):
        gray = int(255 * x / max(1, width - 1))
        draw.line([(x, 0), (x, height - 1)], fill=gray)

    draw.rectangle([(0, 0), (width - 1, height - 1)], outline=0, width=3)
    draw.line([(0, 0), (width - 1, height - 1)], fill=0, width=2)
    draw.line([(0, height - 1), (width - 1, 0)], fill=0, width=2)

    return encode_raw_image(img)


def cmd_upload(client: EpdClient, args: argparse.Namespace) -> None:
    path = Path(args.file)
    filename = args.filename or path.name
    with path.open("rb") as f:
        client.upload(filename, f)
    print(f"Uploaded {path} -> /sdcard/{filename}")


def cmd_convert(client: EpdClient, args: argparse.Namespace) -> None:
    img = Image.open(args.image)
    if args.resize:
        w, h = args.resize
        img = img.resize((w, h))
    raw = encode_raw_image(img)
    Path(args.output).write_bytes(raw)
    print(f"Wrote {args.output} ({img.size[0]}x{img.size[1]}, {len(raw)} bytes)")


def cmd_display_image(client: EpdClient, args: argparse.Namespace) -> None:
    client.display_image(args.filename, args.x, args.y)
    print(f"Placed {args.filename} at ({args.x}, {args.y})")


def cmd_display_text(client: EpdClient, args: argparse.Namespace) -> None:
    client.display_text(args.text, args.x, args.y, args.size)
    print(f"Placed text at ({args.x}, {args.y})")


def cmd_dashboard(client: EpdClient, args: argparse.Namespace) -> None:
    from dashboard import render_dashboard

    img = render_dashboard(now_hour=args.now_hour)
    raw = encode_raw_image(img)
    if args.out:
        Path(args.out).write_bytes(raw)
        print(f"Wrote {args.out} ({len(raw)} bytes)")

    client.upload("layout.bin", io.BytesIO(raw))
    print(f"Uploaded layout.bin ({len(raw)} bytes)")
    client.display_image("layout.bin", 0, 0)
    print("Placed dashboard at (0, 0) -- panel refresh takes a few seconds")


def cmd_chart(client: EpdClient, args: argparse.Namespace) -> None:
    """Live Tibber data by default -- --demo was the old behaviour and made it
    far too easy to 'verify' the display against placeholder numbers."""
    if args.demo:
        from dashboard import PRICES_TODAY, PRICES_TOMORROW
        values = PRICES_TOMORROW if args.slot == "tomorrow" else PRICES_TODAY
        source = "Demo"
    else:
        from hass import Hass
        by_day = Hass().tibber_prices(days=2)
        days = sorted(by_day)
        idx = 1 if args.slot == "tomorrow" else 0
        if idx >= len(days):
            raise EpdClientError(f"no Tibber data for '{args.slot}' yet")
        values = by_day[days[idx]]
        source = f"Tibber {days[idx]}"

    client.display_chart(values, slot=args.slot)
    print(f"Pushed {len(values)} slots for '{args.slot}' from {source} "
          f"(axis + now-marker decided on-device)")


# The task list is still placeholder data -- it will be generated elsewhere.
DEMO_TASKS = [
    {"text": "Firmware-Release taggen", "done": True},
    {"text": "Flash-Backup nach GHCR", "done": True},
    {"text": "Partial-Refresh bauen", "done": False},
    {"text": "Gehaeuse fraesen", "done": False},
    {"text": "Sensor-Board bestellen", "done": False},
]


def cmd_dashboard_live(client: EpdClient, args: argparse.Namespace) -> None:
    """All four cells rendered on-device, fed from Home Assistant."""
    if not args.demo:
        from hass import Hass as _H, build_agenda_payload
        agenda = build_agenda_payload(_H(), slot="top-left")
        client.display_agenda(agenda)
        print(f"top-left : {len(agenda['events'])} Termine, "
              f"{len(agenda['todos'])} Aufgaben, {len(agenda['waste'])} Abfuhrtermine")
    else:
        client.display_list(DEMO_TASKS, slot="top-left", title="Heute zu erledigen")
        print(f"top-left : {len(DEMO_TASKS)} Aufgaben (Beispieldaten)")

    if args.demo:
        from dashboard import PRICES_TODAY, PRICES_TOMORROW
        # Deliberately varied series so the two sparklines can be judged even
        # when the real forecast is flat (e.g. a cloudless day).
        demo_temps = [15, 14, 13, 13, 12, 12, 13, 15, 17, 19, 20, 21,
                      22, 22, 21, 20, 19, 18, 17, 16, 16, 15, 15, 14]
        demo_pop = [0, 0, 0, 5, 10, 20, 35, 55, 70, 80, 65, 40,
                    20, 10, 5, 0, 0, 15, 45, 60, 40, 20, 10, 5]
        weather = {"slot": "top-right", "title": "Wetter (Demo)", "condition": "partlycloudy",
                   "temp": 18.0, "temp_min": 11.0, "temp_max": 22.0, "precip": 2.4,
                   "wind": 12.0, "precip_prob": max(demo_pop),
                   "temp_series": demo_temps, "pop_series": demo_pop,
                   "forecast": [{"label": f"{h}h", "temp": t}
                                for h, t in (("12", 20), ("15", 21), ("18", 19), ("21", 15))]}
        today, tomorrow = PRICES_TODAY, PRICES_TOMORROW
    else:
        from hass import Hass, build_weather_payload
        hass = Hass()
        weather = build_weather_payload(hass, args.weather_entity, slot="top-right")

        import datetime
        by_day = hass.tibber_prices(days=2)
        days = sorted(by_day)
        today = by_day[days[0]]
        tomorrow = by_day[days[1]] if len(days) > 1 else []

    client.display_weather(weather)
    print(f"top-right: {weather['condition']}, {weather['temp']}C")

    client.display_chart(today, slot="today")
    print(f"bot-left : {len(today)} Slots heute (Now-Marker vom Geraet)")

    if tomorrow:
        client.display_chart(tomorrow, slot="tomorrow")
        print(f"bot-right: {len(tomorrow)} Slots morgen")
    else:
        # Day-ahead prices are published around 13:00, often a little later.
        # Push the placeholder rather than skipping: otherwise the cell keeps
        # showing yesterday's curve, which looks like current data.
        client.display_chart_empty(
            slot="tomorrow",
            empty_hint="Preise kommen meist ab ca. 13:00")
        print("bot-right: noch keine Preise fuer morgen -> Platzhalter")

    over = [v for v in list(today) + list(tomorrow) if v > args.y_max]
    if over:
        print(f"\nHinweis: {len(over)} Werte liegen ueber {args.y_max} ct/kWh "
              f"(max {max(over):.1f}) und werden im Chart gekappt und markiert.")


def cmd_upload_demo(client: EpdClient, args: argparse.Namespace) -> None:
    demo_bytes = make_demo_image()
    client.upload("demo.bin", io.BytesIO(demo_bytes))
    print("Uploaded demo.bin")

    client.display_image("demo.bin", 40, 40)
    print("Placed demo.bin at (40, 40)")

    client.display_text("Hello from LilyGo T5-4.7 S3!", 40, 260, size=24)
    print("Placed demo text at (40, 260)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="Device hostname or IP address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)

    sub = parser.add_subparsers(dest="command", required=True)

    p_upload = sub.add_parser("upload", help="Upload a raw file to the SD card")
    p_upload.add_argument("file", help="Local file to upload")
    p_upload.add_argument("--filename", help="Remote filename (defaults to the local basename)")
    p_upload.set_defaults(func=cmd_upload)

    p_convert = sub.add_parser("convert", help="Convert an image to the device's raw format")
    p_convert.add_argument("image", help="Source image (any format Pillow can read)")
    p_convert.add_argument("output", help="Output .bin path")
    p_convert.add_argument("--resize", type=int, nargs=2, metavar=("W", "H"))
    p_convert.set_defaults(func=cmd_convert)

    p_dimg = sub.add_parser("display-image", help="Draw a previously uploaded raw image")
    p_dimg.add_argument("--filename", required=True)
    p_dimg.add_argument("--x", type=int, required=True)
    p_dimg.add_argument("--y", type=int, required=True)
    p_dimg.set_defaults(func=cmd_display_image)

    p_dtext = sub.add_parser("display-text", help="Draw a text label")
    p_dtext.add_argument("--text", required=True)
    p_dtext.add_argument("--x", type=int, required=True)
    p_dtext.add_argument("--y", type=int, required=True)
    p_dtext.add_argument("--size", type=int, default=18)
    p_dtext.set_defaults(func=cmd_display_text)

    p_demo = sub.add_parser("upload-demo", help="Generate + upload + place a demo image and text")
    p_demo.set_defaults(func=cmd_upload_demo)

    p_chart = sub.add_parser("chart", help="Push a price curve; device draws it with LVGL")
    p_chart.add_argument("--slot", choices=("today", "tomorrow"), default="today")
    p_chart.add_argument("--demo", action="store_true",
                         help="Use placeholder data instead of live Tibber prices")
    p_chart.set_defaults(func=cmd_chart)

    p_live = sub.add_parser("dashboard-live",
                            help="All four cells rendered on-device, data from Home Assistant")
    p_live.add_argument("--demo", action="store_true",
                        help="Use placeholder data instead of querying Home Assistant")
    p_live.add_argument("--weather-entity", default="weather.openweathermap",
                        help="Needs precipitation_probability for the rain sparkline; weather.homebw does not provide it")
    p_live.add_argument("--y-max", type=float, default=60.0,
                        help="Device-side chart ceiling, for the clipping hint only")
    p_live.set_defaults(func=cmd_dashboard_live)

    p_dash = sub.add_parser("dashboard", help="Render + push the 2x2 pseudo dashboard")
    p_dash.add_argument("--now-hour", type=int, default=14,
                        help="Hour to highlight in today's price chart (0-23)")
    p_dash.add_argument("--out", help="Also write the raw image to this path")
    p_dash.set_defaults(func=cmd_dashboard)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    client = EpdClient(args.host, args.port)
    try:
        args.func(client, args)
    except (EpdClientError, requests.RequestException) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

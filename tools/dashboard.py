"""
Pseudo dashboard layout for the 960x540 e-paper panel.

Renders a 2x2 grid entirely client-side with Pillow and hands back a
greyscale PIL image; epd_client.py uploads it through the normal
/api/upload + /api/display/image path. Doing the composition here rather
than on-device keeps the firmware's REST surface small (it only knows
"place image" and "place text") while still allowing charts and mixed
typography.

All data is placeholder -- the point is the layout, not the numbers.
"""

from __future__ import annotations

from PIL import Image, ImageDraw, ImageFont

PANEL_W, PANEL_H = 960, 540

# The panel resolves 16 grey levels. Stick to a handful of well-separated
# values so nothing turns into mush after the 8bpp -> 4bpp squash on device.
BLACK, DARK, MID, LIGHT, PALE, WHITE = 0, 60, 120, 180, 225, 255

_FONT_DIR = "/usr/share/fonts/truetype/dejavu"


def _font(name: str, size: int) -> ImageFont.FreeTypeFont:
    try:
        return ImageFont.truetype(f"{_FONT_DIR}/{name}", size)
    except OSError:
        return ImageFont.load_default()


F_TITLE = _font("DejaVuSans-Bold.ttf", 21)
F_BIG = _font("DejaVuSans-Bold.ttf", 44)
F_BODY = _font("DejaVuSans.ttf", 17)
F_SMALL = _font("DejaVuSans.ttf", 13)
F_TINY = _font("DejaVuSans.ttf", 11)


def _panel_frame(d: ImageDraw.ImageDraw, box, title: str):
    """Draws a titled card and returns the inner content box."""
    x0, y0, x1, y1 = box
    d.rectangle([x0, y0, x1, y1], outline=BLACK, width=2)
    d.rectangle([x0, y0, x1, y0 + 32], fill=BLACK)
    d.text((x0 + 12, y0 + 6), title, font=F_TITLE, fill=WHITE)
    return (x0 + 12, y0 + 44, x1 - 12, y1 - 12)


def _draw_tasks(d: ImageDraw.ImageDraw, box):
    x0, y0, x1, y1 = _panel_frame(d, box, "Heute zu erledigen")
    items = [
        ("Firmware-Release taggen", True),
        ("Flash-Backup nach GHCR pushen", True),
        ("Partial-Refresh implementieren", False),
        ("Gehaeuse fraesen (DXF pruefen)", False),
        ("Sensor-Board bestellen", False),
    ]
    y = y0 + 4
    for label, done in items:
        # checkbox
        d.rectangle([x0, y + 2, x0 + 15, y + 17], outline=BLACK, width=2)
        if done:
            d.line([x0 + 3, y + 10, x0 + 7, y + 14], fill=BLACK, width=3)
            d.line([x0 + 7, y + 14, x0 + 12, y + 5], fill=BLACK, width=3)
        d.text((x0 + 26, y), label, font=F_BODY, fill=DARK if done else BLACK)
        if done:  # strike-through
            w = d.textlength(label, font=F_BODY)
            d.line([x0 + 26, y + 11, x0 + 26 + w, y + 11], fill=DARK, width=1)
        y += 30

    d.line([x0, y1 - 26, x1, y1 - 26], fill=LIGHT, width=1)
    d.text((x0, y1 - 20), "2 von 5 erledigt", font=F_SMALL, fill=MID)


def _draw_weather(d: ImageDraw.ImageDraw, box):
    x0, y0, x1, y1 = _panel_frame(d, box, "Wetter heute - Erlangen")

    # Geometry is anchored to y0 throughout: mixing y0- and y1-relative
    # offsets is what made the key/value strip collide with the outlook row.
    icon_cy = y0 + 44
    cx, r = x0 + 52, 26
    d.ellipse([cx - r, icon_cy - r, cx + r, icon_cy + r], outline=BLACK, width=3)
    for i in range(8):
        import math
        a = i * math.pi / 4
        d.line([cx + math.cos(a) * (r + 6), icon_cy + math.sin(a) * (r + 6),
                cx + math.cos(a) * (r + 13), icon_cy + math.sin(a) * (r + 13)],
               fill=BLACK, width=3)
    d.ellipse([cx + 6, icon_cy + 6, cx + 74, icon_cy + 40], fill=WHITE, outline=BLACK, width=3)
    d.ellipse([cx + 30, icon_cy - 8, cx + 92, icon_cy + 36], fill=WHITE, outline=BLACK, width=3)

    d.text((x0 + 158, y0 + 2), "18\u00b0", font=F_BIG, fill=BLACK)
    d.text((x0 + 158, y0 + 56), "heiter, spaeter bewoelkt", font=F_SMALL, fill=BLACK)

    ky = y0 + 96
    for label, value in (("Min / Max", "11\u00b0 / 21\u00b0"),
                         ("Regen", "10 %"),
                         ("Wind", "12 km/h NW")):
        d.text((x0, ky), label, font=F_SMALL, fill=MID)
        d.text((x0 + 112, ky), value, font=F_SMALL, fill=BLACK)
        ky += 18

    sep = y0 + 156
    d.line([x0, sep, x1, sep], fill=LIGHT, width=1)
    slots = [("12h", "20\u00b0"), ("15h", "21\u00b0"), ("18h", "19\u00b0"), ("21h", "15\u00b0")]
    step = (x1 - x0) / len(slots)
    for i, (hour, temp) in enumerate(slots):
        sx = x0 + i * step + 8
        d.text((sx, sep + 6), hour, font=F_SMALL, fill=MID)
        d.text((sx, sep + 22), temp, font=F_BODY, fill=BLACK)


def _draw_price_chart(d: ImageDraw.ImageDraw, box, title: str, prices, highlight_now=None):
    x0, y0, x1, y1 = _panel_frame(d, box, title)

    n = len(prices)
    avg = sum(prices) / n
    # Caption goes ABOVE the plot; putting it at the bottom made it land on
    # top of the hour labels.
    caption = f"\u00d8 {avg:.1f}   max {max(prices):.0f}   min {min(prices):.0f} ct"
    d.text((x1 - d.textlength(caption, font=F_TINY), y0), caption, font=F_TINY, fill=BLACK)

    plot_x0, plot_y0 = x0 + 30, y0 + 20
    plot_x1, plot_y1 = x1, y1 - 18
    lo = 0
    hi = max(prices) * 1.15

    for frac in (0, 0.5, 1.0):
        gy = plot_y1 - frac * (plot_y1 - plot_y0)
        d.line([plot_x0, gy, plot_x1, gy], fill=PALE if frac else LIGHT, width=1)
        d.text((x0, gy - 6), f"{int(round(lo + frac * (hi - lo))):>2}", font=F_TINY, fill=MID)

    slot = (plot_x1 - plot_x0) / n
    bw = slot * 0.74
    for i, p in enumerate(prices):
        bx = plot_x0 + i * slot + (slot - bw) / 2
        bh = (p - lo) / (hi - lo) * (plot_y1 - plot_y0)
        top = plot_y1 - bh
        # Cheap hours light, expensive hours dark -- readable without colour.
        fill = PALE if p < avg * 0.85 else (MID if p < avg * 1.15 else DARK)
        d.rectangle([bx, top, bx + bw, plot_y1], fill=fill, outline=BLACK)
        if highlight_now is not None and i == highlight_now:
            d.rectangle([bx - 2, top - 2, bx + bw + 2, plot_y1], outline=BLACK, width=3)

    d.line([plot_x0, plot_y1, plot_x1, plot_y1], fill=BLACK, width=2)

    slots_per_hour = max(1, n // 24)
    for hour in range(0, 24, 4):
        i = hour * slots_per_hour
        if i >= n:
            break
        lx = plot_x0 + i * slot
        d.text((lx - 7, plot_y1 + 3), f"{hour:02d}", font=F_TINY, fill=MID)

    ay = plot_y1 - (avg - lo) / (hi - lo) * (plot_y1 - plot_y0)
    for dx in range(int(plot_x0), int(plot_x1), 8):
        d.line([dx, ay, dx + 4, ay], fill=BLACK, width=1)


# Plausible day-ahead shapes: night trough, morning ramp, midday solar dip,
# evening peak. Given as hourly anchors and expanded to quarter-hourly slots,
# which is what dynamic tariffs actually settle on these days.
_HOURLY_TODAY = [21, 19, 18, 17, 18, 22, 28, 34, 33, 29, 24, 20,
                 17, 15, 16, 19, 25, 32, 38, 41, 36, 30, 26, 23]
_HOURLY_TOMORROW = [24, 22, 20, 19, 20, 25, 31, 37, 35, 30, 25, 21,
                    18, 16, 15, 17, 22, 29, 35, 39, 44, 37, 31, 27]


def quarter_hourly(hourly, seed=0):
    """Expands 24 hourly anchors into 96 quarter-hourly values by linear
    interpolation plus a small deterministic wobble, so the curve looks like
    real quarter-hour settlement data rather than four copies of each hour."""
    import random
    rnd = random.Random(seed)
    out = []
    n = len(hourly)
    for i in range(n):
        a = hourly[i]
        b = hourly[(i + 1) % n]
        for q in range(4):
            v = a + (b - a) * (q / 4.0)
            out.append(round(max(0.0, v + rnd.uniform(-0.9, 0.9)), 1))
    return out


PRICES_TODAY = quarter_hourly(_HOURLY_TODAY, seed=1)
PRICES_TOMORROW = quarter_hourly(_HOURLY_TOMORROW, seed=2)


def render_top_half(margin: int = 10) -> Image.Image:
    """Top row of the 2x2 grid (tasks + weather) as one image.

    The bottom row is NOT drawn here: those two cells are rendered on-device
    with LVGL via /api/display/chart, so the highlighted quarter-hour comes
    from the panel's own NTP clock instead of this machine's."""
    h = PANEL_H // 2
    img = Image.new("L", (PANEL_W, h), WHITE)
    d = ImageDraw.Draw(img)

    gap = margin
    cw = (PANEL_W - 2 * margin - gap) // 2
    ch = h - margin - margin // 2

    _draw_tasks(d, (margin, margin, margin + cw, margin + ch))
    _draw_weather(d, (margin + cw + gap, margin, PANEL_W - margin, margin + ch))
    return img


def render_dashboard(now_hour: int = 14) -> Image.Image:
    """Full-screen, fully client-side variant (charts included).

    Kept for offline preview and for the image-only path; the live dashboard
    uses render_top_half() plus on-device LVGL charts."""
    img = Image.new("L", (PANEL_W, PANEL_H), WHITE)
    d = ImageDraw.Draw(img)

    m, gap = 10, 8
    cw = (PANEL_W - 2 * m - gap) // 2
    ch = (PANEL_H - 2 * m - gap) // 2

    _draw_tasks(d, (m, m, m + cw, m + ch))
    _draw_weather(d, (m + cw + gap, m, PANEL_W - m, m + ch))
    _draw_price_chart(d, (m, m + ch + gap, m + cw, PANEL_H - m),
                      "Strompreis heute (ct/kWh)", PRICES_TODAY,
                      highlight_now=now_hour * 4)
    _draw_price_chart(d, (m + cw + gap, m + ch + gap, PANEL_W - m, PANEL_H - m),
                      "Strompreis morgen (ct/kWh)", PRICES_TOMORROW)

    return img

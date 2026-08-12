# LilyGo T5-4.7 S3 (V2.3) — LVGL + LilyGo-EPD47 + REST API

ESP-IDF (v5.x) firmware for the LilyGo T5-4.7 S3 (ESP32-S3, ED047TC1 e-paper,
960x540, SD card over SPI). Renders UI with LVGL onto the panel via the
vendored `LilyGo-EPD47` driver,
and exposes a REST API for uploading assets and drawing images/text.

## File structure

```
LilyGo-Screen-4.7-S3/
├── CMakeLists.txt              # top-level ESP-IDF project
├── Makefile                    # idf.py wrapper + `make upload-assets`
├── partitions.csv              # custom partition table (4M app, no spiffs)
├── sdkconfig.defaults          # PSRAM, LVGL, compiler opt, Wi-Fi core pin
├── components/
│   └── lilygo_epd47/           # vendored LilyGo-EPD47 panel driver (see its CMakeLists.txt)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # lvgl managed dependency (epdiy deliberately absent)
│   ├── Kconfig.projbuild       # Wi-Fi creds, pins, task placement, ports
│   ├── app_config.h            # shared constants, raw image format
│   ├── app_main.c              # boot sequence
│   ├── wifi_manager.{c,h}      # STA Wi-Fi (core 0)
│   ├── sd_card.{c,h}           # VFS FAT/SPI mount + SD mutex
│   ├── display_manager.{c,h}   # LilyGo-EPD47 init + refresh task (core 1)
│   ├── lvgl_port.{c,h}         # LVGL init, PSRAM buffers, flush cb, LVGL mutex
│   ├── http_server.{c,h}       # esp_http_server setup (core 0)
│   └── api_handlers.{c,h}      # /api/upload, /api/display/image, /api/display/text
└── tools/
    ├── pyproject.toml          # uv-managed deps (requests, Pillow)
    └── epd_client.py           # CLI client used by `make upload-assets`
```

## Architecture

- **Display**: `LilyGo-EPD47` (vendored in `components/lilygo_epd47`) owns the
  panel; `display_manager.c` holds one full-screen 4bpp framebuffer in PSRAM.
  LVGL never touches the panel directly.
- **UI**: LVGL (`LV_COLOR_DEPTH=8`) renders into PSRAM-backed draw buffers;
  its flush callback converts each flushed area to 8-bit grayscale and
  copies it into that framebuffer. A separate, debounced task performs
  the actual (slow) panel refresh so LVGL flushes stay fast.
- **PSRAM**: the large buffers (LVGL draw buffers, the full-panel grayscale
  scratch buffer, uploaded image pixel data) are placed explicitly via
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. LVGL's own small internal
  allocations (objects, styles) go through `CONFIG_LV_MEM_CUSTOM=y`, which
  in the resolved LVGL component falls back to plain `malloc`/`free` —
  this LVGL Kconfig doesn't expose custom allocator *function names*, only
  the on/off switch — so those spill into PSRAM automatically above
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` via `CONFIG_SPIRAM_USE_MALLOC=y`
  rather than through a dedicated wrapper.
- **Storage**: SD card mounted via VFS FAT/SPI with
  `format_if_mount_failed = false` — a corrupt/blank card is a hard error,
  never silently reformatted.
- **RTOS placement**:
  - Core 0 (PRO_CPU): Wi-Fi driver task (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`)
    and `esp_http_server` (`httpd_config_t.core_id`).
  - Core 1 (APP_CPU): LVGL task and the panel refresh task
    (`xTaskCreatePinnedToCore`).
  - Both are Kconfig options (`APP_TASK_CORE_NETWORK` / `APP_TASK_CORE_DISPLAY`).
- **Init order (load-bearing)**: `display_manager_init()` must run *before*
  `sd_card_init()`. `epd_init()` brings up the LCD_CAM/i80 bus and its GDMA
  channel; doing that after the SD card's SPI+DMA is already running leaves the
  card permanently wedged — it still mounts, but every later access fails with
  `sdmmc_read_blocks failed (0x107)` / `errno=5` until a full power cycle.
  Established by bisect on hardware: with panel init skipped, 10/10 uploads
  succeed; with `epd_init()` alone (no power-on, no refresh, no drawing),
  0/10 succeed; with the panel initialised first, 10/10 succeed again. So this
  is a peripheral/DMA init-order constraint, not a current-draw brownout.
- **Time**: SNTP (`time_sync.c`) with a POSIX `TZ` string, started once Wi-Fi
  is up. Exists so the panel can decide for itself which quarter-hour of a
  price curve is "now".
- **Concurrency**: two dedicated mutexes —
  `sd_card_lock()/unlock()` around all VFS access, and
  `lvgl_port_lock()/unlock()` around all `lv_*` calls made from outside
  the LVGL task (i.e. from HTTP handlers). This is what stops the REST API
  and the render task from touching the SD card or the LVGL object tree
  concurrently.

## REST API

| Method | Path                  | Body |
|--------|-----------------------|------|
| GET    | `/api/health`         | — → `{"status":"ok","wifi_connected":bool}` |
| POST   | `/api/upload?filename=NAME` | raw bytes, streamed straight to `/sdcard/NAME` |
| POST   | `/api/display/image`  | `{"filename": "image.bin", "x": int, "y": int}` |
| POST   | `/api/display/text`   | `{"text": "...", "x": int, "y": int, "size": int}` |
| POST   | `/api/display/chart`  | `{"values": [<1..96 numbers>]}` — see below |
| POST   | `/api/display/list`   | `{"items": [{"text": str, "done": bool}, ...]}` |
| POST   | `/api/display/agenda` | `{"events": [...], "todos": [...], "waste": [...]}` |
| POST   | `/api/display/weather`| `{"condition": <HA slug>, "temp": num, ...}` |

**Raw image format** (`/sdcard/*.bin`, produced by `tools/epd_client.py convert`):

```
offset 0: uint16 width   (little-endian)
offset 2: uint16 height  (little-endian)
offset 4: width*height bytes, 8-bit grayscale, row-major
```

### The whole dashboard is rendered on-device

All four cells of the 2x2 grid are drawn by the firmware on `lv_canvas`
(`ui_card.c` provides the shared frame/palette/grid geometry; `chart.c` and
`widgets.c` the content). Clients send **data, not pixels** — nothing renders
a bitmap on the host any more.

`"slot"` selects the cell: `top-left` | `top-right` | `bottom-left` |
`bottom-right`. Charts accept `today` / `tomorrow` as aliases for the two
bottom cells.

### Data sources (Home Assistant)

`tools/hass.py` pulls live data over the Home Assistant REST API, using
credentials from the gitignored `.env`:

```
HOMEASSISTANT_URL=http://<host>:8123
HOMEASSISTANT_TOKEN=<long-lived access token>
```

- `tibber.get_prices` → quarter-hourly prices. Tibber reports **EUR/kWh**, so
  `hass.py` scales by 100 to the chart's ct/kWh axis rather than doing it on
  the device.
- **Calendars** (`GET /api/calendars/<entity>`): appointments from
  `calendar.daniel`, `calendar.elektro_glaser`, `calendar.glasers` and
  `calendar.geburtstage_2`, taken from *now* over the next 48h, merged across
  all four, sorted by start time, first 6 shown with an origin abbreviation.
  All-day entries (`start.date`) and timed ones (`start.dateTime`) are both
  normalised for sorting.
- **Tasks** (`todo.get_items`, `status: needs_action`): open items from six
  todo lists, max 6 shown, likewise tagged with their origin. Ordering is
  deliberate: everything from `PRIORITY_TODO_LIST` (Elektro-Glaser) comes
  first, then the single most urgent item from each remaining list — otherwise
  one long list fills all six slots and the others never appear. Within a
  list the nearest due date wins and undated items sort last, so **overdue
  items surface first**, which is the point. Note not every list carries due
  dates (`todo.bizzmark` has none at all).
- **Public transport** — the one data source the **device fetches itself**,
  rather than receiving over REST. `departures.c` polls the VGN EFA departure
  monitor (`efa.vgn.de`) over HTTPS every `CONFIG_APP_DEP_REFRESH_S` seconds
  on the network core, so the times stay current between dashboard pushes and
  do not depend on a host being awake. Ported from
  `the78mole/Playground/vgn-abfahrten/vgn_abfahrten.py`.

  Three services are configured by default, mirroring the reference script's
  tags: `S1-NBG`, `S1-BBG` and `285`. The first two are the *same line at the
  same stop in opposite directions*, which is precisely why each entry carries
  its own badge label. Queries sharing a stop fetch it only once — the monitor
  response is ~24 kB, so the three entries cost two requests, not three.

  Direction is matched on `servingLine.liErgRiProj.direction` (`H`/`R`), not
  the displayed destination: the S1 towards Nuremberg shows up variously as
  "Lauf (li Pegn)", "Hartmannshof" and so on, while the direction code is
  stable. Rendering never blocks on the network — drawing code only reads the
  cached copy, and a service with no match renders `--:--` rather than a stale
  time.

- **Waste collection** (`calendar.birkenweg_...`): the next four pickups as
  lettered badges in the bottom-right corner — **R**estmüll, **G**elber Sack,
  **B**iomüll, **P**apier — each with its weekday and date.

- `weather.get_forecasts` (`hourly`, also accepts `daily` / `twice_daily`) plus
  the entity state, condensed into current conditions, today's min/max, 24h
  precipitation, four look-ahead slots, and two 24h hourly series driving the
  sparklines in the weather cell (temperature and precipitation probability).

  **Not every weather integration provides `precipitation_probability`.**
  `weather.homebw` does not (it exposes only `precipitation` in mm),
  `weather.openweathermap` does — hence the latter is the default for
  `--weather-entity`. When the field is absent, `hass.py` simply omits the
  series and the widget drops that sparkline rather than plotting fabricated
  zeros.

```sh
uv run epd_client.py --host <ip> dashboard-live                 # live from HA
uv run epd_client.py --host <ip> dashboard-live --demo          # placeholder data
```

### Charts

`/api/display/chart` deliberately takes **data points only** in the common
case. Everything else has a firmware-side default (`Kconfig` → *Time / Chart*):

- **Fixed y axis `0 .. CONFIG_APP_CHART_Y_MAX`** (default 60 ct/kWh) rather
  than auto-scaling, so a curve looks the same height on a cheap day as on an
  expensive one. Values above the maximum are clamped *and* flagged with a
  marker, so an outlier can never masquerade as exactly `y_max`.

- **Lower bound opens up only when needed.** On an ordinary day the axis
  starts at 0, so the full plot height serves the range that matters. When the
  data actually contains negative prices, the axis drops to
  `CONFIG_APP_CHART_Y_MIN_NEG` (default −10 ct/kWh) and bars grow from a
  labelled zero line — upward when the price costs money, downward when it
  pays. Negative bars are drawn white-with-outline so they stand out from
  merely cheap ones. The floor is capped for the same reason as the ceiling:
  prices have hit −49 ct/kWh, but once they are negative at all the answer is
  "switch everything on", and how far below zero they go changes nothing.

  The ceiling is deliberately a **decision threshold, not a display range**:
  above ~60 ct/kWh you should not be starting a dishwasher or planning to bake
  bread anyway, so how far an outlier overshoots carries no information worth
  screen space — the curve below stays legible instead. Clipped bars are
  therefore expected behaviour, not a bug to be fixed by raising the ceiling;
  the current-price readout in the title bar still reports the true figure
  (which is why it can read "jetzt 72.7 ct" above a bar clipped at 60).
- **`CONFIG_APP_CHART_INTERVAL_MIN`** (default 15 → 96 slots/day), matching
  quarter-hourly dynamic tariffs.
- **Cell geometry** from `"slot": "today" | "tomorrow"`, which picks the
  bottom-left / bottom-right cell of the 2x2 dashboard grid.
- **The highlighted "now" bar is computed by the device** from its own
  SNTP-synced clock (`time_sync.c`), not sent by the client — a client with a
  skewed clock therefore cannot mislabel the display. Without a valid clock
  the chart renders with no highlight and prints `--:--  (no NTP)` instead of
  guessing.

- **An empty `values` array is valid** and renders a framed "Noch keine Daten"
  card instead of a plot. Day-ahead prices for the next day are not published
  before ~13:00 (often later), so `dashboard-live` pushes this placeholder for
  the tomorrow cell rather than skipping the call — otherwise the cell would
  keep displaying the *previous* day's curve, which looks like current data.
  `empty_text` / `empty_hint` override the wording.

Optional overrides: `title`, `x`, `y`, `w`, `h`, `y_max`, `y_min_neg`,
`interval_min`, `highlight_now`, `empty_text`, `empty_hint`.

Drawn on an `lv_canvas` rather than `lv_chart`, because `lv_chart` cannot style
one individual bar differently — and a single canvas object beats ~96 LVGL
objects on a panel this size.

```sh
uv run epd_client.py --host <ip> chart --slot today
uv run epd_client.py --host <ip> dashboard-live   # image top row + LVGL charts
```

Images and text labels are placed unscaled and are **not** garbage
collected — each call adds a new LVGL object/pixel buffer that lives for
the process lifetime. That's intentional for a "compose a screen over
several calls" workflow, but a long-running deployment that places many
distinct images should add eviction (e.g. a `DELETE` endpoint or an LRU)
before going to production.

## Building

Wi-Fi credentials live in a gitignored `.env` at the repo root:

```sh
cat > .env <<'EOF'
WIFI_SSID=your-ssid
WIFI_PASS=your-password
EOF
```

`make build` renders it into `sdkconfig.secrets` (also gitignored) and merges
it over `sdkconfig.defaults` via `SDKCONFIG_DEFAULTS`, so credentials never
enter a tracked file. Then:

```sh
make set-target      # idf.py set-target esp32s3
make build
make flash-monitor   # PORT=/dev/ttyACM0 BAUD=460800 by default
```

Note `BAUD` defaults to 460800: 921600 hangs `read_flash` on this board's
native USB-Serial/JTAG interface (writes are fine, reads are not).

Before overwriting a board that still carries vendor firmware, take a full
dump first — `make backup-flash` writes a 16MB image to `backups/`, and
`make restore-flash BACKUP_FILE=backups/<file>.bin` puts it back.

### Flash dumps live in a registry, not in git

`backups/` is gitignored: 16MB binaries do not belong in git history. Dumps are
published to the GitHub Container Registry as OCI artifacts using `oras`:

```sh
# once, with a PAT carrying write:packages
echo $GITHUB_TOKEN | oras login ghcr.io -u <github-user> --password-stdin

make push-backup GHCR_OWNER=<github-user>   # newest dump in backups/
make pull-backup GHCR_OWNER=<github-user> BACKUP_TAG=20260812
```

`GHCR_OWNER` is derived from the `origin` remote once one exists, so the
explicit argument can be dropped then. `BACKUP_TAG` defaults to today's date;
`BACKUP_FILE=` selects a specific dump instead of the newest. After
`make pull-backup`, `make restore-flash BACKUP_FILE=backups/<file>.bin`
writes it back to the device.

## Asset upload tooling

```sh
cd tools
uv run epd_client.py --host <device-ip> upload-demo
uv run epd_client.py --host <device-ip> convert photo.png photo.bin --resize 480 270
uv run epd_client.py --host <device-ip> upload photo.bin
uv run epd_client.py --host <device-ip> display-image --filename photo.bin --x 20 --y 20
uv run epd_client.py --host <device-ip> display-text --text "Hello" --x 20 --y 300 --size 24
```

`make upload-assets DEVICE_HOST=<device-ip>` generates a procedural demo
image, uploads it, and places both an image and a text label — no bundled
binary assets needed.

## Board revision: this targets V2.3 (PCB dated 2021-6-10)

The pin map and panel driver here are specific to the **V2.3** board and were
recovered from this unit's own factory firmware: the original flash image
contains `LilyGo-EPD47\src\epd_driver.c` path strings, identifying the driver
LilyGo actually shipped. `LilyGo-EPD47`'s `src/utilities.h` then supplied the
authoritative pin map. Verified on hardware: SD mounts, panel refreshes,
REST API serves.

This differs substantially from the later "T5 S3 Pro" board, whose schematic
is what most current LilyGo documentation and the `T5S3-4.7-e-paper-PRO`
repository describe:

| Signal | V2.3 (this board) | S3 Pro |
|---|---|---|
| I2C SDA / SCL | 18 / 17 | 39 / 40 |
| SD MISO / MOSI / SCLK / CS | 16 / 15 / 11 / 42 | 21 / 13 / 14 / 12 |
| Panel power control | shift register (`CFG_DATA`/`CFG_CLK`/`CFG_STR`) | I2C PCA9535 + TPS65185 |

Two consequences worth knowing:

- On V2.3, **GPIO40 is the panel's `STH` line, not I2C SCL**, and GPIO21 is
  `BUTTON_1`, not SD CS. Applying the S3 Pro pin map here produces a
  permanently-low "SCL" and an SD card that never mounts — both look like
  hardware faults but are not.
- **epdiy does not support this board.** `epd_board_lilygo_t5_47` is the
  classic ESP32 I2S path (compiled out on S3, fails to link) and
  `lilygo_board_s3` expects the I2C PMIC this revision lacks. That is why the
  driver is the vendored `LilyGo-EPD47` instead — see
  `components/lilygo_epd47/CMakeLists.txt` for what was vendored, what was
  deliberately left out, and the upstream `ESP_IDF_VERSION_MAJOR` include-order
  bug that had to be worked around.

The V2.3→V2.4 differences are battery/deep-sleep related (leakage, wake-on-battery
crash) and do not affect this USB-powered, always-on project.

## Licensing

- **This project's own code** (`main/`, `tools/`, build files) — MIT, see `LICENSE`.
- **`components/lilygo_epd47/`** — vendored from
  [Xinyuan-LilyGO/LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47),
  licensed **GPL-3.0**; its `LICENSE` and `README.md` stay with those sources.

MIT is GPL-compatible, so the project's own files may be reused under MIT terms
on their own. Note however that the **built firmware image links the GPL-3.0
driver**, so a distributed binary (or a fork that keeps the vendored driver) is
subject to GPL-3.0. If you need an MIT-only artifact, the panel driver would
have to be replaced with a non-GPL implementation.

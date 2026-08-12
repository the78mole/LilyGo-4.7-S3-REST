# lilygo_epd47 (vendored)

Vendored copy of the panel driver sources from
[Xinyuan-LilyGO/LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47)
(`src/`), licensed **GPL-3.0** — see `LICENSE` in this directory.

This is the driver the board's factory firmware was built against, identified
from `LilyGo-EPD47\src\epd_driver.c` path strings recovered from this unit's
original flash image. It targets the 2021-6-10 / V2.3 PCB, whose panel power is
sequenced over a shift-register config bus rather than the I2C PCA9535 +
TPS65185 of the later "S3 Pro" boards — which is why upstream `epdiy` does not
fit this hardware.

**Not vendored** from upstream `src/`:

| File | Why |
|---|---|
| `touch.cpp`, `touch.h` | depend on `Arduino.h`; touch is unused here |
| `font.c` + font headers | pull in `zlib/zlib.h`, which Arduino ships but ESP-IDF does not. Text is rendered by LVGL, and no other translation unit references these symbols. |

**Local modifications** to the vendored sources:

| Change | Why |
|---|---|
| `epd_driver.c`: `provide_out` task priority 10 → 24 (`EPD_PROVIDE_OUT_PRIO`) | The producer runs on core 0 alongside Wi-Fi (23), esp_timer (22) and lwIP (18); at priority 10 it gets preempted, stalling the consumer's blocking `xQueueReceive` mid-frame. The panel holds a row only by residual charge, so a stall shifts rows by several pixels, differently per pass — a smeared, doubled image. Areas ≤64 rows were unaffected because the queue is 64 rows deep. |

See `CMakeLists.txt` for the build wiring, including the workaround for an
upstream `ESP_IDF_VERSION_MAJOR` include-order bug.

Because this GPL-3.0 code is linked into the firmware, the combined work is
subject to GPL-3.0 when distributed.

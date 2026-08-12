# Vendored from https://github.com/Xinyuan-LilyGO/LilyGo-EPD47 (src/).
#
# This is the driver the board's factory firmware was actually built against
# -- proven by "LilyGo-EPD47\src\epd_driver.c" path strings recovered from
# this unit's original flash image, and by a working hello-world on hardware.
# It targets the 2021-6-10 / V2.3 PCB, whose panel power is sequenced over a
# shift-register config bus (CFG_DATA/CFG_CLK/CFG_STR, see ed047tc1.h) rather
# than the I2C PCA9535 + TPS65185 of the later "S3 Pro" boards -- which is why
# upstream epdiy's board definitions do not fit this hardware.
#
# Deliberately NOT vendored from upstream src/:
#   touch.cpp / touch.h  -- depend on Arduino.h; touch is unused here.
#   font.c + font headers -- pull in "zlib/zlib.h", which Arduino ships but
#                            ESP-IDF does not. Text is rendered by LVGL, so
#                            the built-in EPD font engine is not needed. No
#                            other translation unit references its symbols.
#
# Keep SRCS explicit (rather than SRC_DIRS ".") so a future re-vendor cannot
# silently drag those files back in.

idf_component_register(
    SRCS
        "epd_driver.c"
        "ed047tc1.c"
        "i2s_data_bus.c"
        "rmt_pulse.c"
    INCLUDE_DIRS "."
    # ESP-IDF 5.x split the monolithic `driver` component: rmt_tx.h /
    # rmt_encoder.h (used by rmt_pulse.c) now live in esp_driver_rmt, so
    # requiring `driver` alone does not put them on the include path.
    REQUIRES driver esp_driver_rmt esp_driver_gpio esp_timer
    PRIV_REQUIRES esp_lcd
)

# Upstream bug workaround.
#
# ed047tc1.c, epd_driver.c, i2s_data_bus.c and rmt_pulse.c all branch on
# ESP_IDF_VERSION_MAJOR *without* including <esp_idf_version.h> first. The
# macro is therefore undefined (== 0) at the first guard, so rmt_pulse.c takes
# the pre-IDF5 branch and includes the legacy <driver/rmt.h>. That legacy
# header then pulls in esp_idf_version.h itself, so every *later* guard in the
# same file suddenly evaluates true -- leaving new-API code (rmt_channel_handle_t,
# rmt_symbol_word_t, ...) compiled without driver/rmt_tx.h ever being included.
#
# Force-including the version header ahead of every translation unit makes all
# guards agree on the real IDF version. Done here rather than by editing the
# four vendored sources so a future re-vendor from upstream cannot silently
# reintroduce the bug.
target_compile_options(${COMPONENT_LIB} PRIVATE -include esp_idf_version.h)

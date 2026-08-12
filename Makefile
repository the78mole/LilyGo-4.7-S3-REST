# -----------------------------------------------------------------------
# Convenience wrapper around `idf.py` + the Python asset-upload tooling.
# -----------------------------------------------------------------------

IDF_PY       ?= idf.py
PORT         ?= /dev/ttyACM0
# 921600 hangs during read_flash/write_flash on this board's native
# USB-Serial/JTAG interface (confirmed by hand); 460800 is reliable.
BAUD         ?= 460800
DEVICE_HOST  ?= 192.168.1.50

FLASH_SIZE   ?= 0x1000000
BACKUP_DIR   ?= backups

# --- Wi-Fi credentials -------------------------------------------------
# .env (gitignored) holds WIFI_SSID / WIFI_PASS. It is rendered into a
# generated, also-gitignored sdkconfig fragment rather than being written
# into the tracked sdkconfig.defaults, so credentials never reach git.
# ESP-IDF merges every file in SDKCONFIG_DEFAULTS left-to-right, later
# files winning -- so the generated fragment overrides the placeholders.
ENV_FILE     ?= .env
SECRETS_FILE := sdkconfig.secrets

ifneq ($(wildcard $(ENV_FILE)),)
export SDKCONFIG_DEFAULTS := sdkconfig.defaults;$(SECRETS_FILE)
endif

.PHONY: secrets
secrets: $(SECRETS_FILE)

$(SECRETS_FILE): $(ENV_FILE)
	@set -a; . ./$(ENV_FILE); set +a; \
	 printf 'CONFIG_APP_WIFI_SSID="%s"\nCONFIG_APP_WIFI_PASSWORD="%s"\n' \
	   "$$WIFI_SSID" "$$WIFI_PASS" > $@
	@echo "Generated $@ from $(ENV_FILE) (gitignored, not committed)"

.PHONY: all set-target build flash monitor flash-monitor menuconfig \
        clean fullclean size upload-assets backup-flash restore-flash

all: build

set-target:
	$(IDF_PY) set-target esp32s3

build: $(if $(wildcard $(ENV_FILE)),$(SECRETS_FILE))
	$(IDF_PY) build

flash:
	$(IDF_PY) -p $(PORT) -b $(BAUD) flash

monitor:
	$(IDF_PY) -p $(PORT) monitor

flash-monitor:
	$(IDF_PY) -p $(PORT) -b $(BAUD) flash monitor

menuconfig:
	$(IDF_PY) menuconfig

clean:
	$(IDF_PY) clean

fullclean:
	$(IDF_PY) fullclean

size:
	$(IDF_PY) size

# Pushes a generated demo image + a text label to the device over the
# REST API. DEVICE_HOST can be overridden, e.g.:
#   make upload-assets DEVICE_HOST=10.0.0.42
upload-assets:
	cd tools && uv run epd_client.py --host $(DEVICE_HOST) upload-demo

# Dumps the entire flash (FLASH_SIZE, default 16MB) to BACKUP_DIR before
# you overwrite anything. Do this before the first `make flash` on a
# board that came with factory/vendor firmware you might want back.
backup-flash:
	mkdir -p $(BACKUP_DIR)
	esptool.py --port $(PORT) -b $(BAUD) read_flash 0x0 $(FLASH_SIZE) \
		"$(BACKUP_DIR)/flash_backup_$$(date +%Y%m%d_%H%M%S).bin"

# Writes a full-flash dump back, e.g.:
#   make restore-flash BACKUP_FILE=backups/flash_backup_20260812_020614.bin
restore-flash:
	@test -n "$(BACKUP_FILE)" || { echo "Usage: make restore-flash BACKUP_FILE=backups/<file>.bin"; exit 1; }
	esptool.py --port $(PORT) -b $(BAUD) write_flash 0x0 $(BACKUP_FILE)

# --- Flash dumps as OCI artifacts (GitHub Container Registry) ----------
# backups/ is gitignored: 16MB binaries do not belong in git history. They
# are published to GHCR as OCI artifacts via `oras` instead.
#
# GHCR_OWNER defaults to the owner parsed out of the `origin` remote; pass
# it explicitly until a remote exists:
#   make push-backup GHCR_OWNER=the78mole
#
# Authenticate once (a classic PAT needs write:packages):
#   echo $$GITHUB_TOKEN | oras login ghcr.io -u <github-user> --password-stdin
GHCR_REGISTRY     ?= ghcr.io
GHCR_OWNER        ?= $(shell git remote get-url origin 2>/dev/null | sed -E 's#.*[:/]([^/]+)/[^/]+?(\.git)?$$#\1#')
BACKUP_PKG        ?= lilygo-t5-47-s3-flash-backup
BACKUP_TAG        ?= $(shell date +%Y%m%d)
OCI_ARTIFACT_TYPE ?= application/vnd.lilygo.flash-backup.v1
OCI_MEDIA_TYPE    ?= application/vnd.lilygo.flash-backup.v1+octet-stream
# Newest dump in backups/ unless BACKUP_FILE is given.
PUSH_BACKUP_FILE  ?= $(or $(BACKUP_FILE),$(shell ls -1t $(BACKUP_DIR)/*.bin 2>/dev/null | head -1))

.PHONY: push-backup pull-backup

push-backup:
	@test -n "$(GHCR_OWNER)" || { echo "GHCR_OWNER is empty (no origin remote yet) -- pass GHCR_OWNER=<user>"; exit 1; }
	@test -n "$(PUSH_BACKUP_FILE)" || { echo "No dump in $(BACKUP_DIR)/ -- run 'make backup-flash' first"; exit 1; }
	@echo "Pushing $(PUSH_BACKUP_FILE) -> $(GHCR_REGISTRY)/$(GHCR_OWNER)/$(BACKUP_PKG):$(BACKUP_TAG)"
	cd $(BACKUP_DIR) && oras push \
	  $(GHCR_REGISTRY)/$(GHCR_OWNER)/$(BACKUP_PKG):$(BACKUP_TAG) \
	  --artifact-type $(OCI_ARTIFACT_TYPE) \
	  --annotation "org.opencontainers.image.description=Factory flash dump (16MB), LilyGo T5-4.7 S3 PCB 2021-6-10 (V2.3)" \
	  --annotation "org.opencontainers.image.source=https://github.com/$(GHCR_OWNER)/$(notdir $(CURDIR))" \
	  "$(notdir $(PUSH_BACKUP_FILE)):$(OCI_MEDIA_TYPE)"

# Fetches a published dump back into backups/ (then use restore-flash).
pull-backup:
	@test -n "$(GHCR_OWNER)" || { echo "GHCR_OWNER is empty -- pass GHCR_OWNER=<user>"; exit 1; }
	mkdir -p $(BACKUP_DIR)
	cd $(BACKUP_DIR) && oras pull $(GHCR_REGISTRY)/$(GHCR_OWNER)/$(BACKUP_PKG):$(BACKUP_TAG)

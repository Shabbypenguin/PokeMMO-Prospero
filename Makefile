# SPDX-License-Identifier: GPL-3.0-or-later
# pokemmo-prospero. Run these targets inside the build environment (pokemmo-ps5-buildenv):
#   ../pokemmo-ps5-buildenv/ps5env make <target>
SHELL := bash

PROBE_TITLE_ID ?= PPSA27165
# Optional: your PC's LAN address; the probe also sends its log there (it always broadcasts too).
PROBE_LOG_HOST ?=
CLIENT_ZIP ?= private/PokeMMO-Client.zip
# Console FTP server (ftpsrv) for deploy-probe.
PS5_HOST ?=
FTP_PORT ?= 2121

.PHONY: help env-check probe package-probe deploy-probe fetch-client analyze clean

help:
	@echo "make probe          build the hardware probe title  (PROBE_LOG_HOST=192.168.x.y optional)"
	@echo "make package-probe  probe + installer in one zip for players  (dist/pokemmo-prospero-probe-installer.zip)"
	@echo "make deploy-probe   upload it to /data/homebrew over FTP (PS5_HOST=..., FTP_PORT=$(FTP_PORT))"
	@echo "make fetch-client   download the PokeMMO client into $(CLIENT_ZIP) (never committed)"
	@echo "make analyze        check a client release against what the loader supports"
	@echo "make clean          remove build outputs"

env-check:
	@[[ -n "$$PS5_NATIVE_APP_TEMPLATE" && -n "$$PS5_OPENGL_PREFIX" ]] || { \
		echo "Not inside the build environment. Use: ../pokemmo-ps5-buildenv/ps5env make $(MAKECMDGOALS)"; exit 2; }

probe: env-check
	@rm -rf build/probe-assets && mkdir -p build/probe-assets
	@if [[ -n "$(PROBE_LOG_HOST)" ]]; then echo "$(PROBE_LOG_HOST)" > build/probe-assets/loghost.txt; \
	 else echo "(no PROBE_LOG_HOST: UDP broadcast only)" > build/probe-assets/README.txt; fi
	bash scripts/build-title.sh --title-id $(PROBE_TITLE_ID) --name "PokeMMO Prospero Probe" \
		--sources probe --assets build/probe-assets --content-suffix PROBE
	@mkdir -p dist && cp build/titles/$(PROBE_TITLE_ID)/dist/$(PROBE_TITLE_ID).zip dist/pokemmo-prospero-probe-$(PROBE_TITLE_ID).zip
	@echo "Deploy: unzip dist/pokemmo-prospero-probe-$(PROBE_TITLE_ID).zip into /data/homebrew/ on the console"

package-probe: probe
	@rm -rf build/package && mkdir -p build/package/pokemmo-prospero-probe
	cp installer/pokemmo_prospero_install.py installer/install.bat installer/install.command installer/install.sh \
		installer/README.md LICENSE CREDITS.md dist/pokemmo-prospero-probe-$(PROBE_TITLE_ID).zip build/package/pokemmo-prospero-probe/
	cd build/package && rm -f ../../dist/pokemmo-prospero-probe-installer.zip && zip -q -X -r ../../dist/pokemmo-prospero-probe-installer.zip pokemmo-prospero-probe
	@echo "Release zip: dist/pokemmo-prospero-probe-installer.zip (title + installer for Windows/macOS/Linux)"

deploy-probe: env-check
	@[[ -n "$(PS5_HOST)" ]] || { echo "set PS5_HOST to the console's IP"; exit 2; }
	@[[ -d build/titles/$(PROBE_TITLE_ID) ]] || { echo "run 'make probe' first"; exit 2; }
	$(MAKE) -C build/titles/$(PROBE_TITLE_ID) --no-print-directory deploy PS5_HOST=$(PS5_HOST) FTP_PORT=$(FTP_PORT)

fetch-client:
	python3 tools/fetch_client.py --output "$(CLIENT_ZIP)"

analyze:
	@[[ -f "$(CLIENT_ZIP)" ]] || { echo "no client at $(CLIENT_ZIP): run 'make fetch-client' first"; exit 2; }
	python3 tools/analyze_client.py "$(CLIENT_ZIP)"

clean:
	rm -rf build dist

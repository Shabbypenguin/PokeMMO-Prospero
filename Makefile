# SPDX-License-Identifier: GPL-3.0-or-later
# pokemmo-prospero. Run these targets inside the build environment (pokemmo-ps5-buildenv):
#   ../pokemmo-ps5-buildenv/ps5env make <target>
SHELL := bash

PROBE_TITLE_ID ?= PPSA27165
# Homebrew range: retail ids are far lower; PS5 homebrew uses PPSA99xxx (homebrew.page catalog), 98xxx was free.
LOADER_TITLE_ID ?= PPSA98001
# Optional: your PC's LAN address; the probe also sends its log there (it always broadcasts too).
PROBE_LOG_HOST ?=
CLIENT_ZIP ?= private/PokeMMO-Client.zip
# Console FTP server (ftpsrv) for deploy-probe.
PS5_HOST ?=
FTP_PORT ?= 2121

.PHONY: help env-check probe package-probe deploy-probe loader package-loader host-loader host-run overlay-preview updater-test image-loader fetch-client analyze clean

help:
	@echo "make probe          build the hardware probe title  (PROBE_LOG_HOST=192.168.x.y optional)"
	@echo "make package-probe  probe + installer in one zip for players  (dist/pokemmo-prospero-probe-installer.zip)"
	@echo "make deploy-probe   upload it to /data/homebrew over FTP (PS5_HOST=..., FTP_PORT=$(FTP_PORT))"
	@echo "make loader         build the loader title (milestone 1: self-checks + the client up to its graphics setup)"
	@echo "make package-loader loader + installer in one zip (dist/pokemmo-prospero-loader-installer.zip)"
	@echo "make image-loader   the loader as a .ffpfsc image (dist/)"
	@echo "make host-loader    build the same loader for this Linux PC (build/host/prospero-host); host-run runs the client in it"
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

loader: env-check
	@rm -rf build/loader-assets && mkdir -p build/loader-assets
	@if [[ -n "$(PROBE_LOG_HOST)" ]]; then echo "$(PROBE_LOG_HOST)" > build/loader-assets/loghost.txt; \
	 else echo "(no PROBE_LOG_HOST: UDP broadcast only)" > build/loader-assets/README.txt; fi
	bash scripts/runtime-libs.sh build/loader-assets/lib
	cp assets/settings/defaults.properties build/loader-assets/defaults.properties
	bash scripts/build-title.sh --title-id $(LOADER_TITLE_ID) --name "PokeMMO Prospero (dev)" \
		--sources loader/src --sources loader/platform/ps5 --include loader/include --zlib \
		--assets build/loader-assets --content-suffix LOADER --download-mib 2048 \
		--branding assets/branding
	@mkdir -p dist && cp build/titles/$(LOADER_TITLE_ID)/dist/$(LOADER_TITLE_ID).zip dist/pokemmo-prospero-loader-$(LOADER_TITLE_ID).zip

# The same title as one compressed image (.ffpfsc) for ShadowMountPlus, built by the boilerplate's packaging (MkPFS).
image-loader: loader
	$(MAKE) -C build/titles/$(LOADER_TITLE_ID) --no-print-directory ffpfsc
	@mkdir -p dist && cp build/titles/$(LOADER_TITLE_ID)/dist/$(LOADER_TITLE_ID).ffpfsc dist/$(LOADER_TITLE_ID).ffpfsc
	@echo "Image: dist/$(LOADER_TITLE_ID).ffpfsc"

package-loader: loader
	@rm -rf build/package && mkdir -p build/package/pokemmo-prospero-loader
	cp installer/pokemmo_prospero_install.py installer/install.bat installer/install.command installer/install.sh \
		installer/README.md LICENSE CREDITS.md dist/pokemmo-prospero-loader-$(LOADER_TITLE_ID).zip build/package/pokemmo-prospero-loader/
	cd build/package && rm -f ../../dist/pokemmo-prospero-loader-installer.zip && zip -q -X -r ../../dist/pokemmo-prospero-loader-installer.zip pokemmo-prospero-loader
	@echo "Release zip: dist/pokemmo-prospero-loader-installer.zip (developer build: install with --client PokeMMO-Client.zip)"

# The loader on this PC (no console needed): same adapters, platform/host. Needs a C compiler and zlib headers.
host-loader:
	$(MAKE) -f loader/Makefile.host --no-print-directory

# Pictures of the loading screen, keyboard and link box (build/overlay-preview/*.ppm), drawn with Mesa (libegl-dev, libgl-dev).
OVERLAY_SOURCES := loader/src/overlay.c loader/src/overlay_assets.c loader/src/loading_screen.c loader/src/osk.c loader/src/link_box.c loader/src/qrcodegen.c \
	loader/src/roms.c loader/platform/host/platform_host.c
overlay-preview:
	@mkdir -p build/overlay-preview
	$${CC:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Iloader/include -o build/overlay-preview/preview tools/overlay_preview.c \
		$(OVERLAY_SOURCES) -lEGL -lGL -lz -lm
	EGL_PLATFORM=surfaceless build/overlay-preview/preview build/overlay-preview

# The client updater against a local copy of the client zip served by tools/range_server.py (plain HTTP, with drops).
UPDATER_ZIP ?= private/PokeMMO-Client.zip
updater-test:
	@mkdir -p build/updater-test && rm -rf build/updater-test/slot
	$${CC:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Iloader/include -o build/updater-test/updater_test \
		tools/updater_test.c loader/src/updater.c loader/src/diagnostics.c loader/platform/host/platform_host.c -lz -lpthread
	python3 tools/range_server.py $(UPDATER_ZIP) 8765 --drop-every 30000000 & server=$$!; sleep 1; \
	build/updater-test/updater_test http://127.0.0.1:8765/PokeMMO-Client.zip build/updater-test/slot -; status=$$?; \
	kill $$server; exit $$status

host-run: host-loader
	@[[ -f "$(CLIENT_ZIP)" ]] || { echo "no client at $(CLIENT_ZIP): run 'make fetch-client' first"; exit 2; }
	rm -rf build/host/client build/host/root && mkdir -p build/host/client
	unzip -q "$(CLIENT_ZIP)" -d build/host/client -x 'bin/win*' 'bin/mac*' 'bin/linux/arm64/*' '*.exe'
	./build/host/prospero-host --client build/host/client --root build/host/root --timeout 60

fetch-client:
	python3 tools/fetch_client.py --output "$(CLIENT_ZIP)"

analyze:
	@[[ -f "$(CLIENT_ZIP)" ]] || { echo "no client at $(CLIENT_ZIP): run 'make fetch-client' first"; exit 2; }
	python3 tools/analyze_client.py "$(CLIENT_ZIP)"

clean:
	rm -rf build dist

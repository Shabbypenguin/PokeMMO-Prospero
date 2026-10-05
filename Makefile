# pokemmo-ps5 — run targets inside the toolchain image: scripts/ps5env make <target>
SHELL := bash

PROBE_TITLE_ID ?= PPSA27165
# Optional: your PC's LAN address; the probe also sends its log there (it always broadcasts too).
PROBE_LOG_HOST ?=
CLIENT_ZIP ?= private/PokeMMO-Client.zip

# Console FTP (ftpsrv) for deploy-probe.
PS5_HOST ?=
FTP_PORT ?= 2121

.PHONY: help probe deploy-probe analyze clean

help:
	@echo "make probe          build the hardware probe title  (PROBE_LOG_HOST=192.168.x.y optional)"
	@echo "make deploy-probe   upload it to /data/homebrew over FTP (PS5_HOST=..., FTP_PORT=$(FTP_PORT))"
	@echo "make analyze        check a client release           (CLIENT_ZIP=$(CLIENT_ZIP))"
	@echo "make clean          remove build outputs"

probe:
	@rm -rf build/probe-assets && mkdir -p build/probe-assets
	@if [[ -n "$(PROBE_LOG_HOST)" ]]; then echo "$(PROBE_LOG_HOST)" > build/probe-assets/loghost.txt; \
	 else echo "(no PROBE_LOG_HOST: UDP broadcast only)" > build/probe-assets/README.txt; fi
	bash scripts/build-title.sh --title-id $(PROBE_TITLE_ID) --name "PokeMMO PS5 Probe" \
		--sources probe --assets build/probe-assets --content-suffix PROBE
	@mkdir -p dist && cp build/titles/$(PROBE_TITLE_ID)/dist/$(PROBE_TITLE_ID).zip dist/pokemmo-ps5-probe-$(PROBE_TITLE_ID).zip
	@echo "Deploy: unzip dist/pokemmo-ps5-probe-$(PROBE_TITLE_ID).zip to /data/homebrew/ on the console"

deploy-probe:
	@[[ -n "$(PS5_HOST)" ]] || { echo "set PS5_HOST to the console's IP"; exit 2; }
	@[[ -d build/titles/$(PROBE_TITLE_ID) ]] || { echo "run 'make probe' first"; exit 2; }
	$(MAKE) -C build/titles/$(PROBE_TITLE_ID) --no-print-directory deploy PS5_HOST=$(PS5_HOST) FTP_PORT=$(FTP_PORT)

analyze:
	python3 tools/analyze_client.py "$(CLIENT_ZIP)"

clean:
	rm -rf build dist

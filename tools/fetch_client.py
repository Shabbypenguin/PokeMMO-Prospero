#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Adapted from PokeMMO-NX tools/fetch_client.py, Copyright (c) 2026 Petit_Prince, MIT license
# (see LICENSES/PokeMMO-NX-MIT.txt and CREDITS.md).
"""Download the public PokeMMO desktop client into private/ without running it.

The client is never committed or redistributed by this project; every builder fetches their own copy.
"""

import argparse
import hashlib
import json
import urllib.request
import zipfile
from pathlib import Path

URL = "https://dl.pokemmo.com/download/PokeMMO-Client.zip"
MEMBER = "bin/linux/x64/PokeMMO"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=URL)
    parser.add_argument("--output", type=Path, default=Path("private/PokeMMO-Client.zip"))
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_suffix(".part")
    digest = hashlib.sha256()
    request = urllib.request.Request(args.url, headers={"User-Agent": "pokemmo-ps5/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as out:
            resolved = response.url
            while block := response.read(1 << 20):
                out.write(block)
                digest.update(block)
        with zipfile.ZipFile(partial) as archive:
            if MEMBER not in archive.namelist():
                raise ValueError(f"archive has no {MEMBER}")
            revision = archive.read("revision.txt").decode().strip() if "revision.txt" in archive.namelist() else "?"
        partial.replace(args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        partial.unlink(missing_ok=True)
        parser.exit(1, f"download failed: {error}\n")

    provenance = {"requested_url": args.url, "resolved_url": resolved, "revision": revision,
                  "sha256": digest.hexdigest(), "bytes": args.output.stat().st_size}
    args.output.with_suffix(".provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    main()

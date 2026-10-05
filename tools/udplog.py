#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
"""Receive the probe's (and later the loader's) UDP log: python3 tools/udplog.py [--port 18194] [--out probe.log]"""

import argparse
import socket
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=18194)
    parser.add_argument("--out", help="also append every line to this file")
    args = parser.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    receiver.bind(("", args.port))
    print(f"listening on udp/{args.port} (Ctrl+C to stop)", file=sys.stderr)
    log = open(args.out, "a", encoding="utf-8") if args.out else None
    last = None
    try:
        while True:
            data, (host, _) = receiver.recvfrom(4096)
            line = data.decode("utf-8", "replace").rstrip("\n")
            # Broadcast + unicast deliver each line twice when a log host is configured.
            if (host, line) == last:
                continue
            last = (host, line)
            stamped = f"{time.strftime('%H:%M:%S')} {host} {line}"
            print(stamped, flush=True)
            if log:
                log.write(stamped + "\n")
                log.flush()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

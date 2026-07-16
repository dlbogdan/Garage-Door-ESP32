#!/usr/bin/env python3
"""Fail host tests before ESP-IDF's HTTP URI-handler table is exhausted."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "components/management_server/management_server.cpp"
ROUTE_SOURCES = (
    SERVER,
    ROOT / "components/setup_api/setup_api.cpp",
    ROOT / "components/management_api/management_api.cpp",
    ROOT / "components/ota_api/ota_api.cpp",
)
MINIMUM_SPARE_HANDLERS = 4


def main() -> int:
    server_source = SERVER.read_text(encoding="utf-8")
    capacity_match = re.search(r"config\.max_uri_handlers\s*=\s*(\d+)\s*;", server_source)
    if capacity_match is None:
        print("error: could not find config.max_uri_handlers", file=sys.stderr)
        return 1

    capacity = int(capacity_match.group(1))
    route_count = sum(
        len(re.findall(r"\.uri\s*=", source.read_text(encoding="utf-8")))
        for source in ROUTE_SOURCES
    )
    spare = capacity - route_count
    print(f"HTTP routes: {route_count}; capacity: {capacity}; spare: {spare}")

    if spare < MINIMUM_SPARE_HANDLERS:
        print(
            f"error: HTTP server requires at least {MINIMUM_SPARE_HANDLERS} spare "
            "URI-handler slots; increase max_uri_handlers or consolidate routes",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

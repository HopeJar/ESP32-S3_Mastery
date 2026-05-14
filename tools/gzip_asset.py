#!/usr/bin/env python3
"""Create a deterministic gzip copy of a web asset."""

from __future__ import annotations

import gzip
import shutil
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gzip_asset.py <source> <destination>", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    destination = Path(sys.argv[2])
    destination.parent.mkdir(parents=True, exist_ok=True)

    with source.open("rb") as src, destination.open("wb") as raw_dst:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_dst, mtime=0, compresslevel=9) as gz_dst:
            shutil.copyfileobj(src, gz_dst)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

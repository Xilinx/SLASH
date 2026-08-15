#!/usr/bin/env python3
# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

import argparse
import shutil
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Stage the RP1 protocol header into slashkit package data")
    parser.add_argument(
        "--check", action="store_true",
        help="fail instead of updating an absent or stale staged header")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    source = repo_root / \
        "driver/libslash/include/slash/uapi/rp1_protocol.h"
    destination = repo_root / (
        "linker/slashkit/resources/aved/rp1/"
        "include/slash/uapi/rp1_protocol.h"
    )

    if not source.is_file():
        parser.error(f"canonical RP1 protocol header is missing: {source}")

    synchronized = (
        destination.is_file()
        and destination.read_bytes() == source.read_bytes()
    )
    if args.check:
        if not synchronized:
            parser.error(
                "packaged RP1 protocol header is absent or stale; run "
                "scripts/stage-rp1-protocol-header.py")
        return 0

    if not synchronized:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

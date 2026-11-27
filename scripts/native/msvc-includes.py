"""Measure the installed compiler's include prefix instead of guessing its locale."""
from __future__ import annotations

import argparse
import locale
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def include_prefix(output: bytes, marker: str) -> str:
    try:
        text = output.decode("utf-8")
    except UnicodeDecodeError:
        text = output.decode(locale.getpreferredencoding(False), errors="strict")
    matches = []
    for line in text.splitlines():
        if marker not in line:
            continue
        path = re.search(r"(?:[A-Za-z]:[\\/]|\\\\)", line)
        if path is None:
            continue
        prefix = line[:path.start()].rstrip(" \t")
        if prefix and prefix.endswith(":"):
            matches.append(prefix + " ")
    if len(matches) != 1:
        raise RuntimeError("MSVC did not produce one unambiguous include-prefix marker")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("--language", choices=("C", "CXX"), default="CXX")
    args = parser.parse_args()
    if not args.compiler.is_file() or not args.build_directory.is_dir():
        parser.error("An installed compiler and existing owned build directory are required")
    with tempfile.TemporaryDirectory(prefix="nickel-include-prefix-", dir=args.build_directory) as directory:
        probe = Path(directory)
        marker = "nickel_include_prefix_marker.h"
        (probe / marker).write_text("#pragma once\n", encoding="ascii")
        source = probe / ("probe.c" if args.language == "C" else "probe.cpp")
        source.write_text(f'#include "{marker}"\n', encoding="ascii")
        result = subprocess.run(
            [str(args.compiler), "/nologo", "/utf-8", "/Zs", "/showIncludes",
             "/TC" if args.language == "C" else "/TP", str(source)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False,
        )
        if result.returncode:
            raise RuntimeError("The installed MSVC include-prefix probe did not compile")
        prefix = include_prefix(result.stdout, marker)
    sys.stdout.reconfigure(encoding="utf-8")
    print(prefix, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Normalize localized MSVC include records for Ninja's ASCII dependency parser."""
from __future__ import annotations

import os
import subprocess
import sys

PREFIX = b"Note: including file: "
LOCALIZED = "\u6ce8\u610f: \u5305\u542b\u6587\u4ef6: "


def normalize(line: bytes) -> bytes:
    for encoding in ("utf-8", "cp936"):
        prefix = LOCALIZED.encode(encoding)
        if line.startswith(prefix):
            path = line[len(prefix):]
            if encoding != "utf-8":
                path = path.decode(encoding).encode("utf-8")
            return PREFIX + path
    return line


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("Expected a compiler executable and its arguments")
    environment = dict(os.environ)
    environment["VSLANG"] = "1033"
    with subprocess.Popen(sys.argv[1:], env=environment, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as child:
        assert child.stdout is not None
        for line in child.stdout:
            sys.stdout.buffer.write(normalize(line))
            sys.stdout.buffer.flush()
        return child.wait()


if __name__ == "__main__":
    raise SystemExit(main())

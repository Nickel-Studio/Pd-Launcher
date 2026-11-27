#!/usr/bin/env python3
"""Verify the installed static Qt prefix without development DLL/plugin search paths."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = {key.upper(): value for key, value in os.environ.items()} if os.name == "nt" else dict(os.environ)
    for key in list(environment):
        if key.upper().startswith("QT_") or key.upper() in {"QML_IMPORT_PATH", "QML2_IMPORT_PATH"}:
            environment.pop(key)
    if os.name == "nt":
        windows = Path(environment["SYSTEMROOT"])
        environment["PATH"] = os.pathsep.join(str(path) for path in (windows / "System32", windows))
    platforms = ["offscreen", "windows"] if os.name == "nt" else ["offscreen"]
    with tempfile.TemporaryDirectory(prefix="nickel-static-qt-") as directory:
        for platform in platforms:
            result = subprocess.run([str(executable), "-platform", platform], cwd=directory,
                                    env=environment, capture_output=True, text=True, timeout=20)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                return result.returncode
            evidence = json.loads(result.stdout)
            if evidence["qtVersion"] != "6.8.3" or evidence["platform"] != platform:
                raise AssertionError("The installed Qt runtime does not match the requested version/platform")
            if not all(evidence[key] for key in ("widgets", "qtTest", "png", "svg", "markdownNoHtml",
                                                 "webp", "webpLosslessPixels", "webpAlpha", "webpAnimation", "webpStaticPlugin")):
                raise AssertionError("A required Qt capability failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

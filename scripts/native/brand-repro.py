#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Rebuild brand assets into an isolated directory and compare all original bytes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    base = ROOT / ".snow/brand-evidence"
    base.mkdir(parents=True, exist_ok=True)
    expected_path = ROOT / "native/assets/brand/asset-manifest.json"
    expected = json.loads(expected_path.read_text(encoding="utf-8"))
    evidence_path = base / "reproducibility-palladium-v1.json"
    if evidence_path.exists():
        raise ValueError("Refusing to overwrite frozen Palladium reproducibility evidence")
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", QT_QPA_PLATFORM="offscreen")
    generator = "scripts/native/brand-palladium.py"
    with tempfile.TemporaryDirectory(prefix="brand-repro-", dir=base) as temp:
        output = Path(temp)
        argv = [sys.executable, str(ROOT / generator), "rebuild", "--output", output.relative_to(ROOT).as_posix()]
        generated = subprocess.run(argv, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=180)
        if generated.returncode:
            print(generated.stdout + generated.stderr)
            return generated.returncode
        mismatches = []
        for asset in expected["assets"]:
            rebuilt = output / asset["path"]
            if not rebuilt.is_file() or hashlib.sha256(rebuilt.read_bytes()).hexdigest() != asset["sha256"]:
                mismatches.append(asset["path"])
        if expected_path.read_bytes() != (output / "asset-manifest.json").read_bytes():
            mismatches.append("asset-manifest.json")
        evidence = {"revision": expected["revision"], "assetCount": len(expected["assets"]), "byteIdentical": not mismatches,
                    "manifestSha256": hashlib.sha256(expected_path.read_bytes()).hexdigest(), "mismatches": mismatches,
                    "generator": generator, "renderer": expected["renderer"], "argv": argv,
                    "artLicenseConfirmed": expected.get("artLicenseConfirmed", True), "nativeReleaseReady": False}
        evidence_path.write_bytes((json.dumps(evidence, indent=2) + "\n").encode("utf-8"))
        print(json.dumps(evidence, indent=2))
        return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())

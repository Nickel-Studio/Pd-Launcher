#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Run native brand tests without accounts, game files or a real GUI session."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".snow/brand-tools"))
sys.path.insert(0, str(ROOT / "native/tests/brand"))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--functional-only", action="store_true", help="Use stdlib-only immutable baseline/mapping tests")
    parser.add_argument("--evidence", default=".snow/brand-evidence/tests.json")
    args = parser.parse_args()
    pattern = "test_functional.py" if args.functional_only else "test_*.py"
    suite = unittest.defaultTestLoader.discover(str(ROOT / "native/tests/brand"), pattern=pattern)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    evidence = (ROOT / args.evidence).resolve()
    if not evidence.is_relative_to(ROOT):
        raise ValueError("Evidence must remain inside this checkout")
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps({"suite": "native-brand", "functionalOnly": args.functional_only,
                                   "testsRun": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
                                   "skipped": len(result.skipped), "passed": result.wasSuccessful(),
                                   "failureDetails": [str(test) for test, _ in result.failures + result.errors]}, indent=2) + "\n", encoding="utf-8", newline="\n")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())

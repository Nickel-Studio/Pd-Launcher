"""Bounded source checks only; this does not claim compiler, link, or runtime success."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[3]
OWNED = ["native/app/main.cpp", "native/app/AcceptancePaths.cpp", "native/app/AcceptancePaths.hpp",
         "native/app/PerformanceApplication.cpp", "native/app/PerformanceApplication.hpp",
         "native/app/PerformanceProbe.cpp", "native/app/PerformanceProbe.hpp"]


def stripped_cpp(text: str) -> str:
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return tokens.sub(lambda match: "\n" * match.group().count("\n") + " ", text)


def verify() -> dict:
    rows = []
    sources = {}
    for name in OWNED:
        raw = (ROOT / name).read_bytes()
        text = raw.decode("utf-8")
        sources[name] = text
        stack = []
        pairs = {"}": "{", ")": "(", "]": "["}
        for character in stripped_cpp(text):
            if character in "{([":
                stack.append(character)
            elif character in pairs:
                if not stack or stack.pop() != pairs[character]:
                    raise AssertionError(f"Unmatched source delimiter: {name}")
        if stack:
            raise AssertionError(f"Unclosed source delimiters: {name}: {stack}")
        rows.append({"path": name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
                     "delimitersBalanced": True})
    main = sources["native/app/main.cpp"]
    frame = sources["native/app/PerformanceApplication.cpp"]
    paths = sources["native/app/AcceptancePaths.cpp"]
    assert "PerformanceApplication application(argc, argv)" in main
    assert "QCoreApplication::arguments()" in main
    assert main.index("parse_production_arguments(qt_arguments)") < main.index("AppServices services(")
    assert main.index("AcceptancePaths::from_context(") < main.index("AppServices services(")
    assert "else paths = nickel::PathContext::system();" in main
    assert "FinishOnboarding" not in main and "AcceptEula" not in main and "AcknowledgeChangelog" not in main
    assert "ui::MainWindow window(services)" in main and "application.exec()" in main
    assert "DwmFlush(" not in frame and "GdiFlush(" not in frame
    assert "backing_store_flush_complete_ns =" not in frame and "present_dwm_confirmed_ns =" not in frame
    assert "GetDpiForWindow(" in frame
    assert "PathContext::system(" not in paths and "SHGetKnownFolderPath(" not in paths
    assert "explicit-accepted-path-context-v1" in paths and "nickel-evidence-driver-v1" in paths
    assert "require_private_directory" in paths and "GetSecurityInfo(" in paths and "::geteuid()" in paths
    assert "SecureFile::create_new(file)" in sources["native/app/PerformanceProbe.cpp"]
    return {"schemaVersion": 1, "kind": "production-entry-source-check", "status": "source-check-pass",
            "compilerPassed": False, "linked": False, "productionRuntimePassed": False, "files": rows}


if __name__ == "__main__":
    try:
        result = verify()
        print(json.dumps(result, ensure_ascii=True, indent=2))
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)

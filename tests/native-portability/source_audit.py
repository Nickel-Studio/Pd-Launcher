#!/usr/bin/env python3
"""Conservative source-boundary audit; it cannot prove a final binary closure."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re

FORBIDDEN = re.compile(
    r"\b(?:QWeb\w*|QQuick\w*|QQml\w*|QJSEngine|QJSValue|QScriptEngine|"
    r"Qt[56]::(?:WebEngine\w*|WebView\w*|Qml\w*|Quick\w*)|"
    r"WebView2\w*|ICoreWebView2\w*|Cef\w*|JavascriptCore|JavaScriptCore|"
    r"quickjs|duktape|jerryscript|mozjs|v8::|node::)\b|"
    r"#\s*include\s*[<\"][^>\"]*(?:webengine|webview|quickjs|duktape|javascriptcore|v8\.h|node\.h)|"
    r"find_package\s*\(\s*(?:Qt[56][^)]*\b(?:WebEngine|WebView|Qml|Quick)\b|JavaScriptCore|NodeJS)|"
    r"(?:cargo|rustc|npm|pnpm|yarn)\s+(?:build|run|install)", re.I)
PACKAGED_WEB = {".jsx", ".tsx", ".js", ".mjs", ".cjs", ".css", ".qml", ".rcc"}
WIN32_TYPE = re.compile(r"\b(?:HANDLE|HWND|HRESULT|DWORD|LPWSTR|LPSTR|BOOL|SOCKET|HINSTANCE)\b|__declspec\s*\(")
WIN32_INCLUDE = re.compile(r"#\s*include\s*[<\"](?:windows\.h|winsock\w*\.h|bcrypt\.h|shlobj\.h|shellapi\.h|wincrypt\.h)[>\"]", re.I)
QT_CORE_INCLUDE = re.compile(r"#\s*include\s*[<\"](?:Q[A-Z][A-Za-z0-9_]*|QtCore/|QtGui/|QtNetwork/|QtWidgets/)")
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)")
CODE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".c", ".cmake", ".qrc", ".rc", ".manifest"}


def stripped(text: str) -> str:
    # Keep strings for include/link/package checks; comments must not turn
    # explicit 'no WebView' documentation into false production dependencies.
    result = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char in "\"'":
                state = char
                result.append(char)
            elif char == '/' and next_char in {'/', '*'}:
                state = "line" if next_char == '/' else "block"
                result.extend("  ")
                index += 1
            else:
                result.append(char)
        elif state in {'"', "'"}:
            result.append(char)
            if char == '\\' and next_char:
                result.append(next_char)
                index += 1
            elif char == state:
                state = "code"
        elif state == "line":
            result.append('\n' if char == '\n' else ' ')
            if char == '\n':
                state = "code"
        else:
            result.append('\n' if char == '\n' else ' ')
            if char == '*' and next_char == '/':
                result.append(' ')
                index += 1
                state = "code"
        index += 1
    return ''.join(result)


def analyze(path: str, text: str) -> dict:
    code = stripped(text)
    findings = []
    candidates = []
    for number, line in enumerate(code.splitlines(), 1):
        if FORBIDDEN.search(line):
            findings.append({"path": path, "line": number, "kind": "forbidden-runtime-or-build", "text": line.strip()})
        if path.startswith("core/") and QT_CORE_INCLUDE.search(line):
            findings.append({"path": path, "line": number, "kind": "qt-leaks-into-std-core", "text": line.strip()})
        if path.endswith((".h", ".hpp")) and (path.startswith("core/") or path.startswith("platform/")):
            if WIN32_TYPE.search(line) or WIN32_INCLUDE.search(line):
                candidates.append({"path": path, "line": number, "kind": "public-header-win32-candidate", "text": line.strip()})
    stack = []
    system_headers = []
    for number, line in enumerate(code.splitlines(), 1):
        directive = CONDITIONAL.match(line)
        if directive:
            operation, condition = directive.groups()
            if operation in {"if", "ifdef", "ifndef"}:
                stack.append(operation + " " + condition.strip())
            elif operation in {"elif", "else"}:
                if not stack:
                    findings.append({"path": path, "line": number, "kind": "unbalanced-preprocessor", "text": line.strip()})
                else:
                    stack[-1] += " -> " + operation + " " + condition.strip()
            elif stack:
                stack.pop()
            else:
                findings.append({"path": path, "line": number, "kind": "unbalanced-preprocessor", "text": line.strip()})
        if WIN32_INCLUDE.search(line):
            system_headers.append({"path": path, "line": number, "text": line.strip(), "guards": list(stack)})
            if not any(re.search(r"\b(?:_WIN32|WIN32|Q_OS_WIN)\b", item) for item in stack):
                findings.append({"path": path, "line": number, "kind": "unguarded-win32-include", "text": line.strip()})
    if stack:
        findings.append({"path": path, "line": len(code.splitlines()), "kind": "unbalanced-preprocessor", "text": str(stack)})
    return {"findings": findings, "publicHeaderCandidates": candidates, "systemHeaders": system_headers}


def audit(root: Path) -> dict:
    findings = []
    candidates = []
    headers = []
    hashes = []
    skipped = []
    for source in sorted(root.rglob("*")):
        if not source.is_file():
            continue
        relative = source.relative_to(root).as_posix()
        if relative.startswith(("tests/", "assets/")):
            if source.suffix.lower() in PACKAGED_WEB and relative.startswith("assets/"):
                findings.append({"path": relative, "line": 1, "kind": "packaged-web-code", "text": source.suffix})
            continue
        if source.suffix.lower() in PACKAGED_WEB:
            findings.append({"path": relative, "line": 1, "kind": "packaged-web-code", "text": source.suffix})
        if source.suffix.lower() not in CODE_SUFFIXES and source.name != "CMakeLists.txt":
            skipped.append(relative)
            continue
        data = source.read_bytes()
        hashes.append({"path": relative, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        result = analyze(relative, data.decode("utf-8", errors="replace"))
        findings.extend(result["findings"])
        candidates.extend(result["publicHeaderCandidates"])
        headers.extend(result["systemHeaders"])
    fingerprint = hashlib.sha256(json.dumps(hashes, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"schema": 1, "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "root": str(root), "scannerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "sourceHashes": hashes, "sourceFingerprint": fingerprint, "scannedFiles": len(hashes),
            "findings": findings, "publicHeaderCandidates": candidates, "guardedWin32Includes": headers,
            "notScanned": skipped, "passed": not findings and not candidates,
            "scope": "Conservative source-only check; conditional candidates need native compilation; no claim about linked binary/resources/plugins or legacy source outside native."}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.native_root.resolve(strict=True))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
                      "scannedFiles": report["scannedFiles"], "findings": len(report["findings"]),
                      "publicHeaderCandidates": len(report["publicHeaderCandidates"]), "passed": report["passed"]}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Read-only naming/compatibility source checks, never application acceptance."""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
CONTRACT = Path(__file__).with_name("contract.json")
MAX_SOURCE_BYTES = 2 * 1024 * 1024
CPP_PARTS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/')


class SourceError(ValueError):
    pass


def source_path(value: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value or ":" in value or "\0" in value:
        raise SourceError("Source path must be a nonempty repository-relative POSIX path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {".", "..", ".git"} for part in value.split("/")):
        raise SourceError("Source path escapes the repository")
    return str(path)


def cpp_without_comments(text: str) -> str:
    def replace(match: re.Match) -> str:
        value = match.group()
        return re.sub(r"[^\n]", " ", value) if value.startswith(("//", "/*")) else value
    return CPP_PARTS.sub(replace, text)


def line_number(text: str, position: int) -> int:
    return text.count("\n", 0, position) + 1


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def strict_json(value: str | bytes):
    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise SourceError("Duplicate JSON field")
            result[key] = item
        return result
    return json.loads(value, object_pairs_hook=unique)


class RepositorySource:
    def __init__(self, root: Path, ref: str | None = None):
        self.root = root.resolve(strict=True)
        self.cache: dict[str, bytes] = {}
        self.commit = None
        if ref is not None:
            if not re.fullmatch(r"[A-Za-z0-9_./-]{1,200}", ref) or ref.startswith("-"):
                raise SourceError("Invalid Git source reference")
            self.commit = self.git("rev-parse", "--verify", "--end-of-options", ref + "^{commit}").decode().strip()
            if not re.fullmatch(r"[0-9a-f]{40,64}", self.commit):
                raise SourceError("Git did not resolve an immutable commit")

    def git(self, *arguments: str) -> bytes:
        result = subprocess.run(["git", *arguments], cwd=self.root, capture_output=True, timeout=30, check=False)
        if result.returncode:
            raise SourceError("Git source object is unavailable")
        return result.stdout

    def read(self, path: str) -> bytes:
        path = source_path(path)
        if path in self.cache:
            return self.cache[path]
        if self.commit:
            object_name = self.commit + ":" + path
            if int(self.git("cat-file", "-s", object_name)) > MAX_SOURCE_BYTES:
                raise SourceError("Source object exceeds the static-check byte limit")
            data = self.git("show", object_name)
        else:
            destination = self.root.joinpath(*PurePosixPath(path).parts)
            for item in [destination, *destination.parents]:
                if item == self.root:
                    break
                if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
                    raise SourceError("Linked source paths are not inspected")
            if not destination.resolve(strict=True).is_relative_to(self.root):
                raise SourceError("Source resolves outside the repository")
            if not destination.is_file() or destination.stat().st_size > MAX_SOURCE_BYTES:
                raise SourceError("Source is missing, non-regular or exceeds the static-check byte limit")
            with destination.open("rb") as handle:
                data = handle.read(MAX_SOURCE_BYTES + 1)
        if len(data) > MAX_SOURCE_BYTES:
            raise SourceError("Source exceeds the static-check byte limit")
        self.cache[path] = data
        return data

    def provenance(self) -> dict:
        return {"sourceMode": "git-object" if self.commit else "worktree", "sourceCommit": self.commit,
                "reviewedFiles": [{"path": path, "sha256": sha256(data)} for path, data in sorted(self.cache.items())]}


def load_contract(path: Path = CONTRACT) -> dict:
    data = path.read_bytes()
    if len(data) > MAX_SOURCE_BYTES:
        raise SourceError("Contract exceeds the byte limit")
    contract = strict_json(data)
    if not isinstance(contract, dict) or contract.get("schemaVersion") != 1 or not isinstance(contract.get("rules"), list) or not contract["rules"]:
        raise SourceError("Unknown or empty static-check contract")
    identifiers = set()
    for rule in contract["rules"]:
        if rule["id"] in identifiers or rule["category"] not in {"compatibility", "display", "output"}:
            raise SourceError("Duplicate rule or unknown category")
        identifiers.add(rule["id"])
        source_path(rule["path"])
    return contract


def read_text(source, path: str) -> str:
    return source.read(path).decode("utf-8-sig").replace("\r\n", "\n")


def check_rule(source, rule: dict) -> tuple[str | None, int]:
    raw = source.read(rule["path"])
    text = raw.decode("utf-8-sig").replace("\r\n", "\n")
    kind = rule["kind"]
    if kind == "normalized_sha256":
        digest = sha256(raw.replace(b"\r\n", b"\n"))
        return (None if digest == rule["expected"] else "Frozen compatibility fixture changed"), 1
    if kind == "verbatim":
        block = re.search(r"```text\n(.*?)\n```", text, re.S)
        if not block or sha256(block.group(1).encode("utf-8")) != rule["expected"]:
            return "Initial request is missing or changed; put new decisions in an addendum", 1
        return None, line_number(text, block.start(1))
    if kind == "json_equals":
        value = strict_json(text)
        match = re.search(r'"' + re.escape(rule["key"]) + r'"\s*:', text)
        actual = value.get(rule["key"]) if isinstance(value, dict) else None
        return (None if actual == rule["expected"] else "Metadata field differs from naming contract"), line_number(text, match.start()) if match else 1
    code = cpp_without_comments(text) if rule["path"].endswith((".cpp", ".hpp")) else text
    if kind == "cpp_constant":
        pattern = r'\b' + re.escape(rule["name"]) + r'\s*=\s*("(?:\\.|[^"\\])*")\s*;'
        matches = list(re.finditer(pattern, code))
        if len(matches) != 1 or ast.literal_eval(matches[0].group(1)) != rule["expected"]:
            return "Persistent constant is missing, duplicated or renamed", line_number(text, matches[0].start()) if matches else 1
        return None, line_number(text, matches[0].start())
    if kind == "source_contains":
        missing = [token for token in rule["tokens"] if token not in code]
        return ("Required compatibility source token is absent: " + ", ".join(missing)) if missing else None, 1
    if kind == "source_forbids":
        matches = [match for pattern in rule["patterns"] for match in re.finditer(pattern, code, re.M)]
        if matches:
            first = min(matches, key=lambda match: match.start())
            return "New visible/generated product output still uses the historical name", line_number(text, first.start())
        return None, 1
    if kind == "portable_entries":
        parser = cpp_without_comments(read_text(source, rule["parserPath"]))
        choice = re.search(r'const\s+auto\s+name\s*=\s*os\s*==\s*"windows"\s*\?\s*"([^"\n]+)"\s*:\s*"([^"\n]+)"\s*;', code)
        if not choice:
            return "Portable entry selector changed; explicit owner review is required", 1
        windows, linux = choice.groups()
        for historical in ("Nickel.exe", "Nickel-launcher"):
            if '"' + historical + '"' not in parser:
                return "Portable parser removed a legacy entry name", line_number(text, choice.start())
        if windows not in {"Nickel.exe", "pd-launcher.exe"} or linux not in {"Nickel-launcher", "pd-launcher"}:
            return "Portable writer uses an unreviewed entry name", line_number(text, choice.start())
        for selected in (windows, linux):
            if '"' + selected + '"' not in parser:
                return "Portable writer/parser entry names are not statically coordinated", line_number(text, choice.start())
        return None, line_number(text, choice.start())
    raise SourceError("Unknown rule kind")


def audit(source, contract: dict, checks: str = "release") -> dict:
    findings = []
    checked = 0
    for rule in contract["rules"]:
        if checks == "compatibility" and rule["category"] != "compatibility":
            continue
        checked += 1
        try:
            reason, line = check_rule(source, rule)
        except (OSError, ValueError, KeyError, SyntaxError, UnicodeError, subprocess.SubprocessError) as error:
            reason, line = "Static source check unavailable (" + type(error).__name__ + ")", 1
        if reason:
            findings.append({"id": rule["id"], "category": rule["category"], "owner": rule["owner"],
                             "path": rule["path"], "line": line, "reason": reason})
    return {"schemaVersion": 1, "evidenceType": "naming-compatibility-static", "checks": checks,
            "status": "blocked" if findings else "pass", "checkedRules": checked, "passedRules": checked - len(findings),
            "fullApplicationAcceptance": False, "cppParserExecuted": False, "userArtInspected": False,
            "releaseArtifactExecuted": False, "provenance": source.provenance(), "findings": findings,
            "limitations": ["Source tokens do not prove runtime behavior or path/vault safety.",
                            "Portable selector checks do not execute the C++ parser or prove its allowlist.",
                            "No art, screenshots, native builds, binary metadata, migration, or performance acceptance is performed.",
                            "Missing source is a blocker, never an implicit pass. Worktree evidence is identified by per-file hashes."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="Read an immutable commit resolved from this repository, e.g. main")
    parser.add_argument("--checks", choices=("compatibility", "release"), default="release")
    arguments = parser.parse_args()
    try:
        result = audit(RepositorySource(ROOT, arguments.ref), load_contract(), arguments.checks)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "blocked", "fullApplicationAcceptance": False,
                          "error": "Static audit input unavailable (" + type(error).__name__ + ")"}))
        return 2
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())

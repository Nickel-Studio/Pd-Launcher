"""Collect raw native build-input evidence without asserting release completeness."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import ntpath
import os
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET


class EvidenceError(ValueError):
    pass


def path_key(value: str | Path) -> str:
    return ntpath.normcase(ntpath.normpath(str(value))).replace("\\", "/")


def absolute(value: str | Path, base: Path) -> Path:
    path = Path(value)
    return Path(os.path.abspath(path if path.is_absolute() else base / path))


def file_hash(path: Path) -> tuple[str, int]:
    with path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise EvidenceError(f"File changed during capture: {path}")
    return digest, before.st_size


def parse_ninja_deps(text: str) -> dict[str, dict]:
    header = re.compile(r"^(.+): #deps (\d+), deps mtime (\d+) \(([^)]+)\)$")
    result: dict[str, dict] = {}
    current = None
    for line in text.splitlines() + [""]:
        if not line.strip():
            if current is not None:
                if len(current["dependencies"]) != current["declaredCount"]:
                    raise EvidenceError("Truncated Ninja dependency record")
                key = path_key(current["output"])
                if key in result:
                    raise EvidenceError("Duplicate Ninja dependency record")
                result[key] = current
                current = None
            continue
        match = header.fullmatch(line)
        if match:
            if current is not None:
                raise EvidenceError("Unterminated Ninja dependency record")
            current = {"output": match[1], "declaredCount": int(match[2]),
                       "mtime": match[3], "status": match[4], "dependencies": []}
        elif current is not None and line.startswith("    "):
            current["dependencies"].append(line[4:])
        else:
            raise EvidenceError("Unexpected Ninja dependency text")
    return result


def parse_phony_query(text: str) -> set[str]:
    lines = text.splitlines()
    result = set()
    for index, line in enumerate(lines[:-1]):
        if line and not line[0].isspace() and line.endswith(":") and lines[index + 1] == "  input: phony":
            result.add(path_key(line[:-1]))
    return result


def make_words(text: str) -> list[str]:
    result, word = [], []
    index = 0
    while index < len(text):
        character = text[index]
        if character == "\\" and index + 1 < len(text) and text[index + 1] in " \t#:\\":
            word.append(text[index + 1])
            index += 2
            continue
        if character == "$" and index + 1 < len(text) and text[index + 1] == "$":
            word.append("$")
            index += 2
            continue
        if character == "#":
            break
        if character.isspace():
            if word:
                result.append("".join(word))
                word = []
        else:
            word.append(character)
        index += 1
    if word:
        result.append("".join(word))
    return result


def parse_depfile(text: str) -> tuple[list[str], list[str]]:
    text = re.sub(r"\\\r?\n", " ", text).strip()
    delimiter = re.search(r"(?<!\\):(?:[ \t]|$)", text)
    if delimiter is None:
        raise EvidenceError("No Make depfile rule delimiter")
    if re.search(r"\n[^\n]*?(?<!\\):(?:[ \t]|$)", text[delimiter.end():]):
        raise EvidenceError("Multiple depfile rules are not accepted")
    targets = make_words(text[:delimiter.start()])
    dependencies = make_words(text[delimiter.end():])
    if not targets:
        raise EvidenceError("A depfile target is required")
    return targets, dependencies


def ninja_words(text: str) -> list[str]:
    words, current = [], []
    index = 0
    while index < len(text):
        character = text[index]
        if character == "$":
            if index + 1 >= len(text) or text[index + 1] not in " $:":
                raise EvidenceError("Unexpanded Ninja variable in file binding")
            current.append(text[index + 1])
            index += 2
            continue
        if character.isspace():
            if current:
                words.append("".join(current))
                current = []
        else:
            current.append(character)
        index += 1
    if current:
        words.append("".join(current))
    return words


def ninja_edge(text: str, target: str) -> dict:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^build (.+?): (\S+)(?: (.*))?$", line)
        if not match or path_key(target) not in {path_key(word) for word in ninja_words(match[1])}:
            continue
        groups = {"explicit": [], "implicit": [], "orderOnly": []}
        group = "explicit"
        for word in ninja_words(match[3] or ""):
            if word == "|":
                group = "implicit"
            elif word == "||":
                group = "orderOnly"
            else:
                groups[group].append(word)
        variables = {}
        raw = [line]
        for following in lines[index + 1:]:
            if not following.startswith("  "):
                break
            raw.append(following)
            key, separator, value = following.strip().partition(" = ")
            if separator:
                variables[key] = value
        return {"rule": match[2], "inputs": groups, "variables": variables,
                "raw": "\n".join(raw) + "\n", "startLine": index + 1}
    raise EvidenceError(f"Target has no actual Ninja edge: {target}")


def windows_words(text: str) -> list[str]:
    words = []
    index = 0
    while index < len(text):
        while index < len(text) and text[index] in " \t":
            index += 1
        if index == len(text):
            break
        word, quoted = [], False
        while index < len(text) and (quoted or text[index] not in " \t"):
            slashes = 0
            while index < len(text) and text[index] == "\\":
                slashes += 1
                index += 1
            if index < len(text) and text[index] == '"':
                word.extend("\\" * (slashes // 2))
                if slashes % 2:
                    word.append('"')
                else:
                    quoted = not quoted
                index += 1
            else:
                word.extend("\\" * slashes)
                if index < len(text) and (quoted or text[index] not in " \t"):
                    word.append(text[index])
                    index += 1
        if quoted:
            raise EvidenceError("Unclosed quote in a Windows command binding")
        words.append("".join(word))
    return words


def map_libraries(text: str) -> set[str]:
    result = set()
    for line in text.splitlines():
        if re.match(r"^\s+[a-fA-F0-9]{4}:[a-fA-F0-9]+\s+\S+\s+[a-fA-F0-9]{16}", line):
            member = line.split()[-1]
            if ":" in member:
                name = member.split(":", 1)[0]
                if re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
                    result.add(name if name.lower().endswith(".lib") else name + ".lib")
    return result


class Records:
    def __init__(self):
        self.values: dict[str, dict] = {}

    def add(self, path: Path, roles: list[str], expected: dict | None = None) -> dict:
        if not path.is_file() or path.is_symlink():
            raise EvidenceError(f"Missing/nonregular input: {path}")
        digest, size = file_hash(path)
        if expected is not None and (digest != expected["sha256"] or size != expected["bytes"]):
            raise EvidenceError(f"Frozen input changed: {path}")
        key = path_key(path)
        previous = self.values.get(key)
        if previous is not None:
            if (previous["sha256"], previous["bytes"]) != (digest, size):
                raise EvidenceError(f"Conflicting input aliases: {path}")
            previous["roles"] = sorted(set(previous["roles"]) | set(roles))
            return previous
        row = {"path": str(path), "physicalPath": str(path.resolve()), "sha256": digest,
               "bytes": size, "roles": sorted(set(roles))}
        self.values[key] = row
        return row

    def rows(self) -> list[dict]:
        return sorted(self.values.values(), key=lambda row: path_key(row["path"]))


def collect(options: argparse.Namespace) -> dict:
    root, build = options.source_root.absolute(), options.build_root.absolute()
    inventory = json.loads(options.inventory.read_bytes())
    records = Records()
    for row in inventory["records"]:
        records.add(Path(row["path"]), row["roles"], row)
    executable = records.add(Path(inventory["executable"]["path"]), ["actual-linked-executable"], inventory["executable"])
    evidence = []
    options.capture_root.mkdir(parents=True, exist_ok=False)

    def capture(path: Path, role: str) -> dict:
        digest, size = file_hash(path)
        index = len(evidence)
        destination = options.capture_root / f"{index:03d}-{path.name}"
        with path.open("rb") as source, destination.open("xb") as copied:
            shutil.copyfileobj(source, copied, 1024 * 1024)
        if file_hash(destination) != (digest, size) or file_hash(path) != (digest, size):
            raise EvidenceError("Raw evidence changed during capture")
        row = {"path": str(path), "capturePath": str(destination), "sha256": digest, "bytes": size, "role": role}
        evidence.append(row)
        return row

    capture(options.inventory, "previous-checkpoint")
    capture(options.deps_log, "actual-ninja-dependency-query")
    capture(options.query_log, "actual-ninja-phony-query")
    capture(options.inputs_log, "actual-ninja-target-input-query")
    for name in (".ninja_deps", ".ninja_log", "build.ninja", "CMakeFiles/rules.ninja", "compile_commands.json", "CMakeCache.txt"):
        capture(build / name, "actual-build-database-or-commands")
    for path in options.build_evidence:
        capture(path, "actual-build-or-link-command")
    bindings_path = absolute(inventory["compileBindings"]["path"], root)
    capture(bindings_path, "actual-compile-binding-checkpoint")
    bindings = json.loads(bindings_path.read_bytes())["bindings"]
    deps = parse_ninja_deps(options.deps_log.read_text(encoding="utf-8"))
    by_output = {path_key(absolute(row["output"], build)): row for row in deps.values()}
    checked = []
    for binding in bindings:
        output = absolute(binding["output"], build)
        dependency = by_output.get(path_key(output))
        if dependency is None or dependency["status"] != "VALID":
            raise EvidenceError(f"Missing or stale actual object dependencies: {output}")
        source = records.add(absolute(binding["source"], build), ["actual-compiled-source"])
        obj = records.add(output, ["actual-production-object"])
        headers = []
        for name in dependency["dependencies"]:
            row = records.add(absolute(name, build), ["actual-ninja-compiler-dependency"])
            headers.append(row["path"])
        checked.append({**binding, "dependencyStatus": "VALID", "sourceSha256": source["sha256"],
                        "outputSha256": obj["sha256"], "dependencies": headers})
    phony = parse_phony_query(options.query_log.read_text(encoding="utf-8"))
    for node in inventory.get("logicalNonFileInputs", []):
        if path_key(node["logicalNode"]) not in phony:
            raise EvidenceError("An unproven nonfile input cannot be dropped")
    if inventory.get("unresolvedFileInputs"):
        raise EvidenceError("An ordinary file input remains unresolved")
    compiled = {path_key(row["source"]) for row in checked}
    autogen, resources = [], []
    for info_path in sorted(build.rglob("AutogenInfo.json")):
        info = json.loads(info_path.read_bytes())
        if path_key(info["MOC_COMPILATION_FILE"]) not in compiled:
            continue
        capture(info_path, "actual-autogen-command-input")
        generated = Path(info["BUILD_DIR"])
        for key in ("PARSE_CACHE_FILE", "SETTINGS_FILE", "DEP_FILE"):
            if info.get(key):
                capture(Path(info[key]), "actual-autogen-dependency-or-state")
        for category in ("HEADERS", "SOURCES"):
            for item in info.get(category, []):
                records.add(Path(item[0]), ["actual-autogen-scan-input"])
        for name in info.get("CMAKE_LIST_FILES", []):
            records.add(Path(name), ["actual-autogen-generation-input"])
        moc_rows = []
        for dep_path in sorted(generated.rglob("*.d")):
            targets, headers = parse_depfile(dep_path.read_text(encoding="utf-8"))
            if len(targets) != 1:
                raise EvidenceError("MOC depfile must bind one actual generated source")
            target = records.add(absolute(targets[0], build), ["actual-generated-moc-source"])
            capture(dep_path, "actual-moc-depfile")
            for name in headers:
                records.add(absolute(name, build), ["actual-moc-dependency"])
            moc_rows.append({"target": target["path"], "depfile": str(dep_path), "dependencyCount": len(headers)})
        autogen.append({"info": str(info_path), "mocCompilation": info["MOC_COMPILATION_FILE"], "mocDepfiles": moc_rows})
    for info_path in sorted(build.rglob("AutoRcc_*_Info.json")):
        info = json.loads(info_path.read_bytes())
        generated = Path(info["BUILD_DIR"]) / info["OUTPUT_CHECKSUM"] / info["OUTPUT_NAME"]
        if path_key(generated) not in compiled:
            continue
        capture(info_path, "actual-rcc-command-input")
        if info.get("SETTINGS_FILE"):
            capture(Path(info["SETTINGS_FILE"]), "actual-rcc-used-settings")
        qrc = Path(info["SOURCE"])
        records.add(qrc, ["actual-resource-manifest"])
        declared = [absolute((node.text or "").strip(), qrc.parent) for node in ET.fromstring(qrc.read_bytes()).iter("file")]
        if {path_key(path) for path in declared} != {path_key(path) for path in info["INPUTS"]}:
            raise EvidenceError("QRC and actual AUTORCC input lists differ")
        for path in declared:
            records.add(path, ["actual-rcc-embedded-resource"])
        resources.append({"qrc": str(qrc), "generatedSource": str(generated),
                          "commandInfo": str(info_path), "inputs": [str(path) for path in declared]})
    for binding in checked:
        source = Path(binding["source"])
        if source.suffix.lower() == ".rc":
            for name in re.findall(r'^\s*\S+\s+(?:ICON|BITMAP|RCDATA|HTML|TYPELIB)\s+"([^"]+)"', source.read_text(encoding="utf-8"), re.M):
                records.add(absolute(name, source.parent), ["actual-rc-embedded-resource"])
    edge = ninja_edge((build / "build.ninja").read_text(encoding="utf-8"), os.path.relpath(executable["path"], build))
    link_vars = edge["variables"]
    rsp = absolute(link_vars["RSP_FILE"], build)
    if rsp.is_file():
        capture(rsp, "retained-link-response-file")
    map_path = Path(executable["path"]).with_suffix(".map")
    records.add(map_path, ["actual-link-map"])
    observed = map_libraries(map_path.read_text(encoding="utf-8"))
    library_tokens = windows_words(link_vars.get("LINK_LIBRARIES", ""))
    search = [Path(name) for name in os.environ.get("LIB", "").split(os.pathsep) if name]
    known_libraries = {Path(row["path"]).name.casefold(): row["path"] for row in records.rows() if row["path"].lower().endswith(".lib")}
    resolved, unresolved = [], []
    for name in sorted(set(library_tokens) | observed, key=str.casefold):
        if not name.lower().endswith((".lib", ".obj")):
            continue
        direct = absolute(name, build)
        if direct.is_file():
            path, method = direct, "explicit-actual-link-binding"
        elif name.casefold() in known_libraries:
            path, method = Path(known_libraries[name.casefold()]), "actual-input-library-name"
        else:
            path = next((base / name for base in search if (base / name).is_file()), None)
            method = "current-matching-developer-LIB-resolution-not-historical-search-evidence"
        if path is None:
            unresolved.append(name)
            continue
        row = records.add(path, ["actual-link-map-library-or-explicit-link-input"])
        resolved.append({"name": name, "path": row["path"], "sha256": row["sha256"], "resolution": method,
                         "observedInActualMap": name.casefold() in {lib.casefold() for lib in observed}})
    for name in re.findall(r'/MANIFESTINPUT:(?:"([^"]+)"|(\S+))', link_vars.get("LINK_FLAGS", "")):
        records.add(absolute(name[0] or name[1], build), ["actual-linked-manifest"])
    rows = records.rows()
    for row in rows:
        if file_hash(Path(row["path"])) != (row["sha256"], row["bytes"]):
            raise EvidenceError("A file changed before closing the checkpoint")
    return {"schemaVersion": 1, "kind": "raw-actual-native-build-consumption-checkpoint",
            "classification": "not-canonical-release-inventory-not-committed-sourceSnapshot",
            "executable": inventory["executable"],
            "sourceGenerationManifestSha256": inventory["sourceGenerationManifestSha256"],
            "fileSetComplete": False, "compilerDependencyClosureComplete": False,
            "generatedInputsComplete": bool(autogen), "resourceInputsComplete": bool(resources),
            "completenessBlockers": ["CXX system-header consumption was not retained in the original compiler dependency database",
                                     "Original link LIB search environment and response-file bytes were not retained",
                                     "Final full committed-source/source-archive binding is not established"],
            "historicalResponseFileRetained": rsp.is_file(),
            "linkResponseRule": "rspfile_content = $in_newline $LINK_PATH $LINK_LIBRARIES",
            "reconstructedResponseFileNotClaimedHistorical": True,
            "counts": {"files": len(rows), "actualCompilationUnits": len(checked),
                       "provenPhonyNodes": len(inventory.get("logicalNonFileInputs", [])),
                       "actualMocDepfiles": sum(len(row["mocDepfiles"]) for row in autogen),
                       "resourceCompilationUnits": len(resources), "unresolvedLinkLibraries": len(unresolved)},
            "files": rows, "compileBindings": checked, "autogenBindings": autogen,
            "resourceBindings": resources, "linkBindings": resolved, "unresolvedLinkLibraries": unresolved,
            "actualLinkNinjaEdge": edge, "rawEvidence": evidence,
            "roleCounts": dict(Counter(role for row in rows for role in row["roles"]))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source-root", "build-root", "inventory", "deps-log", "query-log", "inputs-log", "capture-root", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--build-evidence", type=Path, action="append", default=[])
    options = parser.parse_args()
    if options.output.exists() or options.capture_root.exists():
        parser.error("Evidence output and raw capture directory must be unused")
    value = collect(options)
    data = (json.dumps(value, indent=2) + "\n").encode("utf-8")
    with options.output.open("xb") as output:
        output.write(data)
    print(json.dumps({"path": str(options.output), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                      "counts": value["counts"], "fileSetComplete": False}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

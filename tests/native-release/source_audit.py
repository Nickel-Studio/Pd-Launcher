"""Read-only immutable legacy and complete multi-owner final source audit."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
from pathlib import Path
import re
import subprocess

from evidence_common import (ROOT, EvidenceError, MissingEvidence, actual_commit, canonical_sha,
                             exit_code, file_ref, gate, inside, new_output, overall, read_git,
                             read_json, relative_name, utc_now, write_json, write_new)

RAW = re.compile(rb":([0-7]{6}) ([0-7]{6}) ([0-9a-f]+) ([0-9a-f]+) ([ACDMRTUXB][0-9]*)\Z")
# Longest exact contract prefix wins; a new C++ file is never inferred to be brand-only.
OWNERS = {
    "native/core/config/": "foundation", "native/platform/": "foundation",
    "native/tests/foundation/": "foundation", "native/core/network/": "transport",
    "native/core/tasks/": "transport", "native/core/archive/": "transport",
    "native/tests/transport/": "transport", "native/core/minecraft/": "minecraft-resume",
    "native/tests/minecraft/": "minecraft-resume", "native/core/accounts/": "identity",
    "native/core/media/": "identity", "native/tests/identity/": "identity",
    "native/core/resources/": "community", "native/core/documents/": "community",
    "native/tests/community/": "community", "native/core/instances/": "instances",
    "native/core/packs/": "instances", "native/core/diagnostics/": "instances",
    "native/tests/instances/": "instances", "native/ui/": "ui-resume",
    "native/tests/ui/": "ui-resume", "native/assets/brand/": "brand-release",
    "native/tests/brand/": "brand-release", "scripts/native/brand": "brand-release",
    "docs/native/licenses/": "brand-release",
    "tests/native-acceptance/": "acceptance-resume", "tests/native-release/": "release-evidence",
    "docs/native/audit.md": "release-evidence",
    "native/app/": "application-wire", "native/tests/app/": "application-wire", "native/core/common/": "lead",
    "tests/native-storage/": "build-storage",
    "native/core/cleanup/": "cleanup-resume", "native/tests/cleanup/": "cleanup-resume",
    "native/core/diagnostics/junk.cpp": "cleanup-resume", "native/core/diagnostics/junk.hpp": "cleanup-resume",
    "tests/native-cleanup/": "cleanup-resume",
    "tests/native-portability/": "portability-audit",
    "scripts/native/": "build-resume", "cmake/": "build-resume",
    "CMakeLists.txt": "build-resume", "CMakePresets.json": "build-resume",
    "CMakeUserPresets.json": "build-resume", "native/CMakeLists.txt": "build-resume",
    "native/metadata.json": "build-resume",
    "native/README.md": "build-resume", "native/assets/": "build-resume",
    "docs/native/": "build-resume", "docs/": "build-resume", ".github/": "build-resume",
    "README.md": "build-resume", "CONTRIBUTING.md": "build-resume", "SECURITY.md": "build-resume",
    "NOTICE.md": "build-resume", "LICENSES/": "build-resume", "website/": "build-resume",
    ".gitignore": "lead",
    ".gitattributes": "lead", "DESIGN.md": "brand-release", "CHANGELOG": "build-resume",
}


def classify(path: str, ownership: dict = OWNERS) -> str | None:
    matches = [prefix for prefix in ownership if path == prefix or
               (prefix.endswith("/") and path.startswith(prefix)) or
               prefix in {"scripts/native/brand", "CHANGELOG"} and path.startswith(prefix)]
    return ownership[max(matches, key=len)] if matches else None


def parse_raw(raw: bytes) -> list[dict]:
    if not raw:
        return []
    if not raw.endswith(b"\0"):
        raise EvidenceError("Truncated Git raw delta")
    tokens, cursor, rows = raw[:-1].split(b"\0"), 0, []
    while cursor < len(tokens):
        matched = RAW.fullmatch(tokens[cursor])
        if matched is None:
            raise EvidenceError("Malformed Git raw delta record")
        old_mode, new_mode, old_object, new_object, status = [part.decode("ascii") for part in matched.groups()]
        count = 2 if status[0] in {"R", "C"} else 1
        cursor += 1
        if cursor + count > len(tokens):
            raise EvidenceError("Missing rename/copy endpoint")
        paths = [relative_name(value.decode("utf-8")) for value in tokens[cursor:cursor + count]]
        rows.append({"status": status, "oldMode": old_mode, "newMode": new_mode,
                     "oldObject": old_object, "newObject": new_object, "paths": paths})
        cursor += count
    return rows


def tree_entries(root: Path, candidate: str) -> list[dict]:
    raw = read_git(root, "--no-replace-objects", "ls-tree", "-r", "-z", "--full-tree", candidate)
    rows, names = [], set()
    for token in raw.rstrip(b"\0").split(b"\0") if raw else []:
        header, name = token.split(b"\t", 1)
        mode, kind, object_id = header.decode("ascii").split(" ")
        path = relative_name(name.decode("utf-8"))
        if path.casefold() in names:
            raise EvidenceError("Case-colliding source tree")
        names.add(path.casefold())
        if kind != "blob" or mode not in {"100644", "100755"}:
            raise EvidenceError(f"Nonregular source entry needs an explicit contract: {path}")
        rows.append({"path": path, "mode": mode, "gitObject": object_id})
    return sorted(rows, key=lambda row: row["path"])


def source_snapshot(root: Path, candidate: str) -> list[dict]:
    """Rehash unique immutable blobs with bounded batch readers, not per-file processes."""
    entries = tree_entries(root, candidate)
    if not 1 <= len(entries) <= 20000:
        raise EvidenceError("Committed source snapshot needs 1..20000 regular files")
    object_ids = list(dict.fromkeys(entry["gitObject"] for entry in entries))
    request = "".join(object_id + "\n" for object_id in object_ids).encode("ascii")
    try:
        checked = subprocess.run(["git", "--no-replace-objects", "cat-file", "--batch-check"], cwd=root,
                                 input=request, capture_output=True, check=False, timeout=30)
    except subprocess.TimeoutExpired as error:
        raise MissingEvidence("Timed out reading committed blob sizes for source mapping") from error
    headers = checked.stdout.decode("ascii").splitlines()
    if checked.returncode or len(headers) != len(object_ids):
        raise EvidenceError("Missing or incomplete Git blob size inventory")
    sizes, total = {}, 0
    for object_id, header in zip(object_ids, headers):
        fields = header.split()
        if len(fields) != 3 or fields[:2] != [object_id, "blob"]:
            raise EvidenceError("Source mapping size inventory identifies a missing/non-blob object")
        size = int(fields[2])
        if not 0 <= size <= 512 * 1024 * 1024 or total + size > 512 * 1024 * 1024:
            raise EvidenceError("Committed source blob bytes exceed the bounded mapping read")
        sizes[object_id] = size
        total += size
    try:
        process = subprocess.run(["git", "--no-replace-objects", "cat-file", "--batch"], cwd=root,
                                 input=request, capture_output=True, check=False, timeout=90)
    except subprocess.TimeoutExpired as error:
        raise MissingEvidence("Timed out rehashing committed source mapping blobs") from error
    if process.returncode:
        raise EvidenceError(process.stderr.decode("utf-8", errors="replace").strip())
    raw, cursor, objects = process.stdout, 0, {}
    for object_id, size in sizes.items():
        end = raw.find(b"\n", cursor)
        if end < 0 or raw[cursor:end].decode("ascii") != f"{object_id} blob {size}":
            raise EvidenceError("Truncated or mismatched Git blob batch header")
        cursor = end + 1
        data = raw[cursor:cursor + size]
        if len(data) != size or raw[cursor + size:cursor + size + 1] != b"\n":
            raise EvidenceError("Truncated committed Git blob bytes")
        objects[object_id] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": size}
        cursor += size + 1
    if cursor != len(raw):
        raise EvidenceError("Unexpected trailing Git blob output")
    return [{**entry, **objects[entry["gitObject"]]} for entry in entries]


def baseline_files(document: dict, expected_count: int) -> dict:
    files = document.get("files") if isinstance(document, dict) else None
    if not isinstance(files, dict) or len(files) != expected_count:
        raise EvidenceError(f"Frozen legacy baseline must contain exactly {expected_count} file records")
    result, names = {}, set()
    for path, value in files.items():
        path = relative_name(path)
        if path.casefold() in names:
            raise EvidenceError("Duplicate/case-colliding frozen path")
        names.add(path.casefold())
        digest = value if isinstance(value, str) else value.get("sha256") if isinstance(value, dict) else None
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise EvidenceError(f"Frozen source lacks SHA-256: {path}")
        result[path] = {"sha256": digest}
        if isinstance(value, dict) and "bytes" in value:
            result[path]["bytes"] = value["bytes"]
    return result


def audit(root: Path, baseline: Path, base: str, candidate: str, *, expected_count: int = 442,
          verify_worktree: bool = True, ownership: dict = OWNERS, provenance: Path | None = None) -> tuple[dict, bytes]:
    base, candidate = actual_commit(root, base), actual_commit(root, candidate)
    document = read_json(baseline)
    frozen = baseline_files(document, expected_count)
    baseline_ref = file_ref(baseline, root)
    declared_base = document.get("baselineCommit")
    if (document.get("kind") != "frozen-legacy-source-and-visual-baseline" or not isinstance(declared_base, str) or
            not re.fullmatch(r"[0-9a-f]{7,64}", declared_base) or not base.startswith(declared_base)):
        raise EvidenceError("The frozen physical baseline must identify the exact named legacy oracle")
    provenance_ref = None
    if provenance is not None:
        capture = read_json(provenance)
        if (not isinstance(capture, dict) or capture.get("schemaVersion") != 1 or
                capture.get("kind") != "authorized-read-only-main-input-transfer" or capture.get("sha256") != baseline_ref["sha256"] or
                capture.get("bytes") != baseline_ref["bytes"] or capture.get("sourceFileCount") != expected_count or
                capture.get("originalManifestCurrentCommit") != document.get("currentCommit") or
                capture.get("originalBaselineCommit") != declared_base or
                capture.get("sourceFilesManifestSha256") != canonical_sha(document["files"])):
            raise EvidenceError("Transferred raw baseline does not match its actual authorization provenance")
        actual_commit(root, capture.get("mainCheckpointAtAuthorization"))
        provenance_ref = file_ref(provenance, root)
    entries, oracle_entries = tree_entries(root, candidate), {row["path"]: row for row in tree_entries(root, base)}
    observed = {}
    for row in entries:
        data = read_git(root, "cat-file", "blob", row["gitObject"])
        observed[row["path"]] = {**row, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    missing, source_changes, physical_differences, unexplained, worktree_drift = [], [], [], [], []
    oracle_files, physical_files, physical_observations = [], [], []
    for path, expected in sorted(frozen.items()):
        physical_files.append({"path": path, **expected})
        original, current = oracle_entries.get(path), observed.get(path)
        if original is None or current is None:
            missing.append(path)
        if original is not None:
            raw = read_git(root, "cat-file", "blob", original["gitObject"])
            oracle_record = {**original, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}
            oracle_files.append(oracle_record)
            if current is not None and (current["gitObject"], current["mode"]) != (original["gitObject"], original["mode"]):
                source_changes.append({"path": path, "oracle": oracle_record, "candidate": current})
            if oracle_record["sha256"] != expected["sha256"] or "bytes" in expected and oracle_record["bytes"] != expected["bytes"]:
                argv = ["check-attr", "-z", "--source=" + base, "text", "eol", "filter", "working-tree-encoding", "--", path]
                attrs_raw = read_git(root, *argv)
                tokens = attrs_raw.rstrip(b"\0").split(b"\0")
                if len(tokens) != 12 or any(tokens[index].decode("utf-8") != path for index in range(0, 12, 3)):
                    raise EvidenceError("Incomplete oracle checkout attribute evidence")
                attrs = {tokens[index + 1].decode("utf-8"): tokens[index + 2].decode("utf-8") for index in range(0, 12, 3)}
                if set(attrs) != {"text", "eol", "filter", "working-tree-encoding"}:
                    raise EvidenceError("Oracle checkout attributes are ambiguous")
                crlf = raw.replace(b"\n", b"\r\n")
                converted = {"sha256": hashlib.sha256(crlf).hexdigest(), "bytes": len(crlf)}
                justified = (current is not None and current["gitObject"] == original["gitObject"] and current["mode"] == original["mode"] and
                             attrs["text"] in {"auto", "set"} and attrs["eol"] in {"unspecified", "crlf"} and
                             attrs["filter"] == "unspecified" and attrs["working-tree-encoding"] == "unspecified" and
                             b"\0" not in raw and b"\r" not in raw and b"\n" in raw and converted["sha256"] == expected["sha256"] and
                             ("bytes" not in expected or converted["bytes"] == expected["bytes"]))
                physical_differences.append({"path": path, "frozenPhysicalBytes": expected, "oracleGitBlob": oracle_record,
                                             "candidateGitBlob": current, "oracleAttributes": attrs, "oracleAttributesGitArgv": ["git", *argv],
                                             "oracleAttributesRawHex": attrs_raw.hex(), "exactLfToCrlfBytes": converted,
                                             "explanationVerified": justified,
                                             "note": "Raw oracle/candidate and original physical identities remain separate; no bytes or hashes are normalized away."})
                if not justified:
                    unexplained.append(path)
        observation = {"path": path, "status": "pending", "actual": None}
        if verify_worktree:
            try:
                actual = file_ref(inside(root, path, must_exist=True), root)
                matches = actual["sha256"] == expected["sha256"] and ("bytes" not in expected or actual["bytes"] == expected["bytes"])
                observation.update({"status": "passed" if matches else "failed", "actual": actual})
                if not matches:
                    worktree_drift.append({"path": path, "expected": expected, "actual": actual})
            except (OSError, EvidenceError) as error:
                observation.update({"status": "failed", "error": str(error)})
                worktree_drift.append({"path": path, "error": str(error)})
        physical_observations.append(observation)
    args = ["diff-tree", "--raw", "-z", "--no-commit-id", "--no-abbrev", "-r", "-M", "-C", base, candidate, "--"]
    raw = read_git(root, *args)
    changes, unmapped, legacy_changes = parse_raw(raw), [], []
    for change in changes:
        change["owners"] = {}
        for path in change["paths"]:
            owner = classify(path, ownership)
            change["owners"][path] = owner
            if owner is None:
                unmapped.append(path)
            if path in frozen:
                legacy_changes.append(path)
            if path in observed:
                change.setdefault("candidateFiles", []).append(observed[path])
    gates = {
        "legacyCommittedBytes": gate("failed" if missing or source_changes else "passed", "All frozen candidate Git blobs/modes equal exact oracle Git blobs/modes", missing=missing, modified=source_changes),
        "frozenPhysicalRepresentation": gate("failed" if missing or unexplained else "passed", "Distinct original physical hashes retained; exact LF-to-CRLF explanations independently recorded", unexplained=unexplained, representationDifferences=physical_differences),
        "legacyWorktreeBytes": gate("failed" if worktree_drift else "passed" if verify_worktree else "pending", f"All {expected_count} actual original physical files compared byte-for-byte", differences=worktree_drift, checkedCount=len(physical_observations) if verify_worktree else 0, observationRoot=str(root.resolve())),
        "ownership": gate("failed" if unmapped else "passed", "Complete multi-owner delta, not a brand-only filter", unmapped=sorted(set(unmapped))),
    }
    inventories = {
        "oracleGit": {"schemaVersion": 1, "kind": "frozen-legacy-oracle-git-inventory", "base": base, "fileCount": len(oracle_files),
                      "filesSha256": canonical_sha(oracle_files), "files": oracle_files, "missing": sorted(set(frozen) - set(oracle_entries)),
                      "identityBasis": "Unchanged raw oracle Git blob bytes and modes; not Windows checkout bytes."},
        "originalPhysical": {"schemaVersion": 1, "kind": "frozen-legacy-original-physical-inventory", "baseline": baseline_ref,
                             "baselineProvenance": provenance_ref, "fileCount": len(physical_files), "filesSha256": canonical_sha(physical_files),
                             "files": physical_files, "observations": physical_observations, "observationRoot": str(root.resolve()),
                             "identityBasis": "Original frozen physical byte hashes remain verbatim; actual checkout observations are separate."},
    }
    result = {"schemaVersion": 1, "kind": "native-final-source-audit", "scope": "observed-source-snapshot", "createdUtc": utc_now(),
              "base": base, "candidate": candidate, "baseTree": read_git(root, "rev-parse", base + "^{tree}").decode("ascii").strip(),
              "candidateTree": read_git(root, "rev-parse", candidate + "^{tree}").decode("ascii").strip(),
              "baseline": baseline_ref, "baselineProvenance": provenance_ref, "baselineCount": len(frozen), "inventories": inventories,
              "sourceSnapshotSha256": canonical_sha(list(observed.values())), "sourceFiles": list(observed.values()),
              "gitArgv": ["git", *args], "rawDeltaSha256": hashlib.sha256(raw).hexdigest(), "changes": changes,
              "changedLegacyPaths": sorted(set(legacy_changes)),
              "ownerCounts": dict(Counter(owner for row in changes for owner in set(row["owners"].values()))),
              "ownershipContractSha256": canonical_sha(ownership), "gates": gates, "status": overall(gates), "nativeReleaseReady": False,
              "limitations": ["Separate complete oracle-Git and original-physical inventories retain all byte identities and ten representation differences.",
                              "This named committed snapshot does not capture dirty/new final build-source bytes or generated production inputs.",
                              "Actual buildSourceSha256 and source/legal/archive correspondence must be bound separately to the final executable."]}
    return result, raw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True,
                        help="Caller-supplied baseline source inventory JSON")
    parser.add_argument("--baseline-provenance", help="Actual authorized raw-input transfer sidecar; historical metadata stays verbatim")
    parser.add_argument("--base", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--no-worktree-check", action="store_true")
    args = parser.parse_args()
    output = new_output(args.output)
    try:
        provenance = inside(ROOT, args.baseline_provenance, must_exist=True) if args.baseline_provenance else None
        result, raw = audit(ROOT, inside(ROOT, args.baseline, must_exist=True), args.base, args.candidate,
                            verify_worktree=not args.no_worktree_check, provenance=provenance)
        write_new(output / "source-diff.raw", raw)
        documents = result.pop("inventories")
        result["inventories"] = {}
        for key, document in documents.items():
            path = output / ("oracle-git-inventory.json" if key == "oracleGit" else "original-physical-inventory.json")
            write_json(path, document)
            result["inventories"][key] = file_ref(path, ROOT)
    except MissingEvidence as error:
        result = {"schemaVersion": 1, "kind": "native-final-source-audit", "status": "pending", "nativeReleaseReady": False,
                  "gates": {"source": gate("pending", str(error))}}
    except (OSError, EvidenceError, KeyError, TypeError, AttributeError) as error:
        result = {"schemaVersion": 1, "kind": "native-final-source-audit", "status": "failed", "nativeReleaseReady": False,
                  "gates": {"source": gate("failed", str(error))}}
    write_json(output / "source-evidence.json", result)
    print(f"{result['status']}: {output.relative_to(ROOT).as_posix()}/source-evidence.json")
    return exit_code(result["status"])


if __name__ == "__main__":
    raise SystemExit(main())

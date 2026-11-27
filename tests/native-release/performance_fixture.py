"""Bounded physical runtime deployment and lawful existing PathContext fixtures."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path

from evidence_common import (ROOT, EvidenceError, MissingEvidence, canonical_sha, check_space, file_ref,
                             inside, read_json, reference, relative_name, sha_file, write_json)
from release_evidence import RUNTIME_ROLES

NATIVE_ROOTS = {"config": "system-config/studio.nickel.launcher", "game": ".minecraft", "cache": "cache", "temp": "temp"}
LEGACY_ROOTS = {"config": "Nickel", "game": ".minecraft", "cache": "cache", "temp": "temp"}
CONFIG_ENTRIES = ("settings.json", "accounts.json", "active_account.json", "game.json", "instances.json")


def fixture_root_names(launcher: dict, *, native: bool) -> dict[str, str]:
    values = launcher.get("fixtureDataRoots", NATIVE_ROOTS if native else LEGACY_ROOTS)
    if not isinstance(values, dict) or set(values) != set(NATIVE_ROOTS):
        raise EvidenceError("Fixture needs exact config/game/cache/temp relative roots")
    result = {key: relative_name(value) for key, value in values.items()}
    names = [value.casefold() for value in result.values()]
    for index, name in enumerate(names):
        if any(name == other or name.startswith(other + "/") or other.startswith(name + "/") for other in names[index + 1:]):
            raise EvidenceError("Fixture data roots collide or overlap")
    if native and Path(result["config"]).name != "studio.nickel.launcher":
        raise EvidenceError("Native config root must preserve the existing application identifier")
    return result


def runtime_layout(root: Path, manifest: dict) -> tuple[list[dict], dict]:
    rows = manifest.get("files")
    if not isinstance(rows, list) or not rows:
        raise MissingEvidence("Actual full runtime file list is required before physical fixture deployment")
    runtime, names = [], set()
    for row in rows:
        if not isinstance(row, dict):
            raise EvidenceError("Runtime manifest records must be objects")
        if row.get("role") not in RUNTIME_ROLES:
            continue
        name = relative_name(row.get("distributionPath"))
        folded = name.casefold()
        if folded in names or any(folded.startswith(existing + "/") or existing.startswith(folded + "/") for existing in names):
            raise EvidenceError("Runtime deployment paths collide")
        names.add(folded)
        path = reference(root, row)
        runtime.append({**row, "distributionPath": name, "bytes": path.stat().st_size})
    executable = manifest.get("executable")
    reference(root, executable)
    applications = [row for row in runtime if row["role"] == "application"]
    if len(applications) != 1 or any(applications[0].get(key) != executable.get(key) for key in ("path", "sha256", "bytes")):
        raise EvidenceError("Physical deployment requires the exact final EXE once, never an installer or renamed hash alias")
    return runtime, applications[0]


def prepare_runtime_fixture(root: Path, manifest: dict, fixture: Path, launcher: dict, *, max_bytes: int) -> dict:
    try:
        fixture = inside(root, fixture.relative_to(root.resolve()).as_posix(), must_exist=True)
    except ValueError as error:
        raise EvidenceError("Physical fixture must remain inside its owned evidence root") from error
    runtime, application = runtime_layout(root, manifest)
    native = manifest.get("kind") == "native-release"
    names = fixture_root_names(launcher, native=native)
    data_roots = {key: str(inside(fixture, value)) for key, value in names.items()}
    executable = inside(fixture, application["distributionPath"])
    receipt_path = inside(fixture, "runtime-deployment.evidence.json")
    total = sum(row["bytes"] for row in runtime)
    binding = {"sourceCommit": manifest.get("sourceCommit"), "product": manifest.get("product"),
               "sourceSnapshotSha256": manifest.get("sourceSnapshotSha256"), "buildSourceSha256": manifest.get("buildSourceSha256"),
               "executableSha256": manifest["executable"]["sha256"], "executablePath": str(executable),
               "fixtureRoot": str(fixture), "dataRoots": data_roots, "runtimeBytes": total}

    def compression(path: Path) -> tuple[int, str]:
        attributes = getattr(path.stat(), "st_file_attributes", 0)
        mode = "ntfs-compressed" if attributes & 0x800 else "uncompressed" if os.name == "nt" else "not-windows"
        if os.name == "nt" and mode != "uncompressed":
            raise EvidenceError("Paired startup needs uncompressed deployment; inherited NTFS compression is not a saving")
        return attributes, mode

    if receipt_path.exists():
        previous = read_json(receipt_path)
        if (not isinstance(previous, dict) or previous.get("kind") != "actual-owned-runtime-fixture-deployment" or
                previous.get("status") != "prepared-not-runtime-accepted" or any(previous.get(key) != value for key, value in binding.items())):
            raise EvidenceError("Warmup deployment belongs to a different product/source/path generation")
        deployed = previous.get("deployedFiles")
        if (not isinstance(deployed, list) or len(deployed) != len(runtime) or
                previous.get("runtimeFilesSha256") != canonical_sha(deployed)):
            raise EvidenceError("Warmup deployment must retain its exact complete physical runtime inventory")
        for row, original in zip(deployed, runtime):
            target = reference(root, row)
            source = reference(root, original)
            if (target != inside(fixture, original["distributionPath"]) or row.get("source") != file_ref(source, root) or
                    any(row.get(key) != original.get(key) for key in ("distributionPath", "role", "sha256", "bytes")) or
                    target.samefile(source)):
                raise EvidenceError("Warmup deployment changed/aliased its actual runtime files")
            _, mode = compression(target)
            if row.get("compressionMode") != mode:
                raise EvidenceError("Warmup runtime compression changed between measured launches")
        return previous
    if type(max_bytes) is not int or not 0 < total <= max_bytes:
        raise EvidenceError("Physical runtime exceeds the explicit remaining owned-output budget")
    check_space(fixture, total)
    targets = [inside(fixture, row["distributionPath"]) for row in runtime]
    if any(target.exists() for target in targets):
        raise EvidenceError("Physical runtime never replaces a fixture/user file")
    if native and inside(fixture, "nickel.portable.json").exists():
        raise EvidenceError("Native acceptance fixture must use seeded system-config selection, not a portable override")
    if native and not any(inside(fixture, names["config"] + "/" + name).is_file() for name in CONFIG_ENTRIES):
        raise MissingEvidence("Native fixture must contain a frozen configuration under studio.nickel.launcher")
    for value in data_roots.values():
        Path(value).mkdir(parents=True, exist_ok=True)
    deployed = []
    for row, target in zip(runtime, targets):
        source = reference(root, row)
        target.parent.mkdir(parents=True, exist_ok=True)
        before = source.stat()
        digest, count = hashlib.sha256(), 0
        with source.open("rb") as input_stream, target.open("xb") as output_stream:
            while chunk := input_stream.read(256 * 1024):
                count += len(chunk)
                if count > row["bytes"]:
                    raise EvidenceError("Runtime source grew during bounded physical copy")
                check_space(target, len(chunk))
                output_stream.write(chunk)
                digest.update(chunk)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        after = source.stat()
        if (count != row["bytes"] or digest.hexdigest() != row["sha256"] or
                (before.st_ino, before.st_size, before.st_mtime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns)):
            raise EvidenceError("Actual runtime identity changed during physical fixture deployment")
        attributes, mode = compression(target)
        deployed.append({**file_ref(target, root), "distributionPath": row["distributionPath"], "role": row["role"],
                         "source": file_ref(source, root), "fileAttributes": attributes, "compressionMode": mode})
    if sha_file(executable) != manifest["executable"]["sha256"]:
        raise EvidenceError("Prepared executable is not the exact measured Artifact")
    result = {"schemaVersion": 1, "kind": "actual-owned-runtime-fixture-deployment", "status": "prepared-not-runtime-accepted",
              **binding, "deployedFiles": deployed, "runtimeFilesSha256": canonical_sha(deployed), "nativeReleaseReady": False,
              "limitations": ["Physical preparation is not actual AcceptancePaths/OS-root acceptance.",
                              "Allocation/compression attributes are not download-package savings."]}
    write_json(receipt_path, result)
    return result

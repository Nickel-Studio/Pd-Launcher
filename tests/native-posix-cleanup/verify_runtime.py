#!/usr/bin/env python3
"""Close recorded native evidence without rerunning or simulating cleanup."""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import zipfile

import verify_snapshot as inputs


CASE = "quarantine-same-inode-content-change-restored-mtime-conflict"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def text(path):
    return path.read_text(encoding="utf-8-sig")


def exit_code(path, expected=0):
    match = re.fullmatch(r"(?:runner_)?exit=(-?\d+)\s*", text(path))
    require(match is not None and int(match[1]) == expected, f"exit mismatch: {path}")
    return expected


def fixture_record(raw):
    created = re.findall(r"^FIXTURE_CREATE (\S+) dev=(\d+) ino=(\d+) uid=1000 mode=0700 filesystem=tmpfs limit=1048576$", raw, re.M)
    released = re.findall(r"^FIXTURE_RELEASE (\S+) highwater_logical=(\d+) highwater_allocated=(\d+) nofollow=true$", raw, re.M)
    require(created and len(created) == len(released), "fixture creation/release count mismatch")
    require({entry[0] for entry in created} == {entry[0] for entry in released}, "fixture exact-path release mismatch")
    require("FIXTURE_CLEANUP_FAILED" not in raw, "native fixture cleanup failure")
    require(all(not os.path.lexists(entry[0]) for entry in created), "an exact native fixture still exists")
    logical = max(int(entry[1]) for entry in released)
    allocated = max(int(entry[2]) for entry in released)
    require(logical <= 1048576 and allocated <= 1048576, "fixture exceeds the 1 MiB bound")
    return {"created": len(created), "released": len(released), "maxLogicalBytes": logical,
            "maxAllocatedBytes": allocated, "filesystem": "tmpfs", "remainingExactPaths": 0, "nofollow": True}


def file_record(path, root):
    require(path.is_file() and not path.is_symlink(), f"not an exact regular evidence file: {path}")
    return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size, "sha256": inputs.digest(path)}


def matrix_result(directory, name, count, mode, root):
    path = directory / (name + ".test.log")
    raw = text(path)
    cases = re.findall(r"^PASS (.+)$", raw, re.M)
    require(len(cases) == count and len(set(cases)) == count and not re.search(r"^FAIL ", raw, re.M), "native matrix case totals changed")
    require(f"ACCEPTANCE_SUMMARY passed={count} failed=0 fixture_cleanup_failures=0" in raw and mode in raw, "native matrix summary/mode missing")
    return {"passed": count, "failed": 0, "exit": exit_code(directory / (name + ".exit.txt")),
            "cases": cases, "log": file_record(path, root), "fixtures": fixture_record(raw)}


def capacity_record(directory, phases):
    raw = text(directory / "capacity.log")
    records = [{"phase": phase, "utc": utc, "C_free": int(c), "D_free": int(d)}
               for phase, utc, c, d in re.findall(r"CAPACITY phase=(\S+) utc=(\S+) C_free=(\d+) D_free=(\d+)", raw)]
    require([entry["phase"] for entry in records] == phases, "phase capacity evidence missing")
    require(raw.count("CAPACITY_PASS") == len(records) and "CAPACITY_BLOCKED" not in raw, "runtime reserve was blocked")
    require(all(entry["C_free"] >= 1073741824 and entry["D_free"] >= 268435456 for entry in records), "runtime reserve fell below the authorized threshold")
    return records


def input_record(directory, source, manifest, manifest_sha, root):
    hashes = [inputs.digest(directory / name) for name in ("inputs-before.txt", "inputs-pretest.txt", "inputs-after.txt")]
    require(len(set(hashes)) == 1, "source/test input fingerprints changed during runtime")
    require(inputs.digest(directory / "snapshot.json") == manifest_sha, "runtime manifest is not the selected root")
    marker = json.loads(text(directory / "publication-complete.json"))
    require(marker["publicationComplete"] is True and marker["snapshotFileSha256"] == manifest_sha, "runtime completion marker mismatch")
    require(marker["sourceFiles"] == 24 and marker["sourceBytes"] == sum(entry["bytes"] for entry in manifest["files"].values()), "runtime source count/length mismatch")
    cache = text(directory / "CMakeCache.txt")
    require(f"NICKEL_POSIX_SOURCE_ROOT:PATH={source}" in cache and f"NICKEL_POSIX_MANIFEST_SHA256:STRING={manifest_sha}" in cache, "runtime CMake source binding mismatch")
    require(f"production_only={source}" in text(directory / "source-roots.txt"), "runtime source root mismatch")
    require(f"BUILD_SOURCES_PASS translation_units=11 production_units=9 test_units=2 production_only={source}" in text(directory / "build-inputs.txt"), "runtime production compilation closure mismatch")
    frozen = re.findall(r"^TEST_SHA256 ([0-9a-f]{64}) (tests/native-posix-cleanup/\S+) bytes=(\d+)$", text(directory / "inputs-before.txt"), re.M)
    require(frozen and len({entry[1] for entry in frozen}) == len(frozen), "frozen owned test inputs missing")
    for sha, name, size in frozen:
        path = directory / "test-inputs" / Path(name).name
        require(inputs.digest(path) == sha and path.stat().st_size == int(size), "archived tested owner input differs from executed bytes")
    host = text(directory / "host.txt")
    require("HOST_PREFLIGHT_PASS" in host and "microsoft-standard-WSL2" in host and "13.3.0" in host and 'VERSION_ID="24.04"' in host, "actual WSL2 Ubuntu/GCC host evidence missing")
    return {"sourceRoot": source.relative_to(root).as_posix(), "sourceFiles": 24, "sourceBytes": marker["sourceBytes"],
            "snapshotSha256": manifest_sha, "unchangedInputsSha256": hashes[0], "compiledTranslationUnits": 11,
            "productionCompilations": 9, "ownedTestCompilations": 2, "archivedTestFiles": len(frozen)}


def elf_record(directory, name, root):
    path = directory / name
    record = file_record(path, root)
    with path.open("rb") as stream:
        require(stream.read(4) == b"\x7fELF", "native evidence is not an ELF")
    require(any(line.startswith(record["sha256"] + "  ") and line.endswith("/" + name)
                for line in text(directory / "executable-sha256.txt").splitlines()), "archived ELF differs from executed SHA")
    return record


def historical_record(root):
    owner = root / ".snow/posix-cleanup-acceptance"
    path = owner / "evidence/fix1-runtime-evidence.json"
    require(inputs.digest(path) == "2e642d63309a18b2369764a8a477fcaf1d0e496857e468bce900171db10f40cd", "historical fix1 runtime metadata changed")
    record = json.loads(text(path))
    archived = owner / "evidence/run-20261121T160023Z-61117/test-inputs"
    for entry in record["inventory"]:
        actual = root / entry["path"]
        if entry["path"].startswith("tests/native-posix-cleanup/"):
            actual = archived / Path(entry["path"]).name
        require(inputs.digest(actual) == entry["sha256"] and actual.stat().st_size == entry["bytes"], "historical source/ELF/log/test-input changed")
    baseline = root / ".snow/native-shared"
    with contextlib.redirect_stdout(io.StringIO()):
        inputs.verify_root(baseline, root, "3ba46d370b2aaef6c3929cc5f3161a4f6b7a3c4c52b5b140454f3aadde67d0b4")
    bundle = owner / "evidence/fix1-delivery.zip"
    require(inputs.digest(bundle) == "9333f753fdb77e6d694920438453d9ddcceccce9e6871349b1f0c3f9fd8b4140", "historical owned delivery changed")
    with zipfile.ZipFile(bundle) as archive:
        raw = archive.read("docs/native/reports/posix-cleanup-acceptance.md")
        require(hashlib.sha256(raw).hexdigest() == "7f67f494717ff1ebc29dedc5827f7ffc4f0d8eb975977f44b9693f74ce026f65", "historical report bytes changed")
    return {"record": file_record(path, root), "inventoryFilesVerified": len(record["inventory"]),
            "oldFailureAndFix1SourceElfLogBytesPreserved": True, "oldReportFromFrozenBundleVerified": True}


def collect(args):
    root = args.worktree.resolve(strict=True)
    owner = root / ".snow/posix-cleanup-acceptance"
    source = args.source_root.resolve(strict=True)
    matrix = args.matrix_evidence.resolve(strict=True)
    repro = args.reproduction_evidence.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    for path in (source, matrix, repro, build):
        path.relative_to(owner)
    manifest_sha = inputs.digest(source / "snapshot.json")
    with contextlib.redirect_stdout(io.StringIO()):
        manifest = inputs.verify_root(source, root, manifest_sha)
        inputs.verify_canonical_source1(source, manifest)
        inputs.verify_build(build, source, manifest, root)
    plain = matrix_result(matrix, "posix_cleanup_plain", 19, "PLAIN_PRODUCTION_NO_TEST_HOOKS", root)
    controlled = matrix_result(matrix, "posix_cleanup_controlled", 35, "CONTROLLED_PRODUCTION_HOOKS_AND_LD_OBSERVERS", root)
    matrix_inputs = input_record(matrix, source, manifest, manifest_sha, root)
    repro_inputs = input_record(repro, source, manifest, manifest_sha, root)
    for phase in ("configure", "build", "runner", "residual"):
        exit_code(matrix / (phase + ".exit.txt"))
    exit_code(repro / "build.exit.txt")
    exit_code(repro / "runner.exit.txt")
    plain_elf = elf_record(matrix, "posix_cleanup_plain", root)
    controlled_elf = elf_record(matrix, "posix_cleanup_controlled", root)
    require(elf_record(repro, "posix_cleanup_controlled", root)["sha256"] == controlled_elf["sha256"], "repetitions used a different ELF")
    require(inputs.digest(build / "compile_commands.json") == inputs.digest(matrix / "compile_commands.json") == inputs.digest(repro / "compile_commands.json"), "executed and current build translation commands differ")
    for name, archived in (("posix_cleanup_plain", plain_elf), ("posix_cleanup_controlled", controlled_elf)):
        require(inputs.digest(build / name) == archived["sha256"], "current build ELF differs from archived executed ELF")
    attempts = []
    for index in range(1, 6):
        path = repro / f"case-{index}.log"
        raw = text(path)
        values = re.search(r"RESTORED_MTIME ino=(\d+) size=3 mtime=(\d+:\d+) ctime_before=(\d+:\d+) ctime_after=(\d+:\d+)", raw)
        require(values is not None and "HOOK_HIT regular.quarantined=1" in raw and "old_content=OLD new_content=NEW" in raw, "real same-inode mutation hook evidence missing")
        same_ctime = values[3] == values[4]
        require(f"same_ctime={int(same_ctime)}" in raw and "CONTENT_RACE_RESULT accepted=0 original_path_exists=1 quarantine_count=0" in raw, "content mutation was not rejected and retained")
        require(f"PASS {CASE}\n" in raw and "ACCEPTANCE_SUMMARY passed=1 failed=0 fixture_cleanup_failures=0" in raw, "native repetition oracle failed")
        if same_ctime:
            require("REJECTION code=revision_conflict message=Quarantined file content changed before deletion" in raw, "same-tick mutation did not reach final content rehash rejection")
        else:
            require("REJECTION code=revision_conflict" in raw, "natural next-tick mutation not rejected")
        attempts.append({"attempt": index, "inode": int(values[1]), "size": 3, "mtimeBeforeAndAfter": values[2],
                         "ctimeBefore": values[3], "ctimeAfter": values[4], "sameCtime": same_ctime,
                         "hookHits": 1, "accepted": False, "newContentRetained": True, "quarantineCount": 0,
                         "exit": exit_code(repro / f"case-{index}.exit.txt"), "log": file_record(path, root), "fixtures": fixture_record(raw)})
    require(any(entry["sameCtime"] for entry in attempts), "no genuine same-kernel-tick mutation reached")
    require("MINIMAL_REPRO_SUMMARY attempts=5 safety_failures=0 no_sleep=true production_source_unchanged=true" in text(repro / "runner.log"), "five-attempt no-sleep receipt missing")
    residual_path = matrix / "residual-boundary.log"
    residual = text(residual_path)
    require("hook_hit=1 same_uid=1000 substitution_deleted=true authorized_original_retained=true" in residual
            and "classification=KNOWN_LIMIT_NOT_SAFETY_PASS" in residual
            and "RESIDUAL_SUMMARY reproduced=1 failed=0 fixture_cleanup_failures=0 NOT_SECURITY_PASS" in residual,
            "same-UID last-unlink residual must stay explicitly excluded from security PASS")
    blocked = None
    if args.blocked_evidence:
        directory = args.blocked_evidence.resolve(strict=True)
        directory.relative_to(owner / "evidence")
        exit_code(directory / "runner.exit.txt", 3)
        require("CAPACITY_BLOCKED" in text(directory / "runner.log") and not (directory / "configure.exit.txt").exists() and not list(directory.glob("*.test.log")), "reserve-blocked attempt must not claim compilation/test results")
        blocked = {"runner": file_record(directory / "runner.log", root), "exit": 3, "beforeConfigureAndFixtures": True}
    files = [path for directory in (source, matrix, repro) for path in directory.rglob("*") if path.is_file() and "__pycache__" not in path.parts]
    if args.blocked_evidence:
        files.extend(path for path in args.blocked_evidence.rglob("*") if path.is_file())
    inventory = [file_record(path, root) for path in sorted(set(files))]
    owner_bytes = sum(path.stat().st_size for path in owner.rglob("*") if path.is_file() and not path.is_symlink())
    require(owner_bytes <= 134217728, "owner exceeds the 128 MiB bound")
    return {"schemaVersion": 1, "owner": "posix-cleanup-acceptance", "generation": "canonical-mutation-source1",
            "classification": "independent-native-runtime-pass-with-explicit-known-limit",
            "scope": "WSL2 Ubuntu24.04 GCC13.3 cleanup and RegularFileToken; 11 current transport plus 13 retained old dependencies, not full app/main Foundation/Windows/Release security PASS.",
            "secureFsCppSha256": manifest["files"]["native/core/tasks/secure_fs.cpp"]["sha256"],
            "matrix": matrix.relative_to(root).as_posix(), "reproductions": repro.relative_to(root).as_posix(),
            "input": matrix_inputs, "reproductionInput": repro_inputs, "plain": plain, "controlled": controlled,
            "elf": {"plain": plain_elf, "controlled": controlled_elf},
            "minimalReproductions": {"attempts": 5, "safetyFailures": 0, "sameCtimeAttempts": sum(entry["sameCtime"] for entry in attempts), "noSleep": True, "cases": attempts},
            "knownLimit": {"classification": "KNOWN_LIMIT_NOT_SAFETY_PASS", "sameUid": 1000, "substitutionDeleted": True,
                           "countedInSafetyPasses": False, "absoluteRaceFreeClaim": False, "log": file_record(residual_path, root), "fixtures": fixture_record(residual)},
            "phaseCapacities": {"matrix": capacity_record(matrix, ["preflight", "configure", "build", "posix_cleanup_plain", "posix_cleanup_controlled", "residual"]),
                                "reproductions": capacity_record(repro, ["preflight", "build", "repro-1", "repro-2", "repro-3", "repro-4", "repro-5"])},
            "reserveBlockedAttempt": blocked, "historical": historical_record(root), "inventory": inventory}


def main():
    parser = argparse.ArgumentParser()
    for name in ("worktree", "source-root", "matrix-evidence", "reproduction-evidence", "build-dir", "output-prefix"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--blocked-evidence", type=Path)
    parser.add_argument("--verify-existing", action="store_true")
    args = parser.parse_args()
    args.output_prefix.resolve().relative_to(args.worktree.resolve() / ".snow/posix-cleanup-acceptance/evidence")
    record_path = Path(str(args.output_prefix) + "-evidence.json")
    index_path = Path(str(args.output_prefix) + "-fingerprints.txt")
    if not args.verify_existing:
        require(not record_path.exists() and not index_path.exists(), "refusing to overwrite completed runtime evidence")
    record = collect(args)
    if args.verify_existing:
        recorded = json.loads(text(record_path))
        require(record == recorded, "actual native evidence differs from its frozen closure")
    else:
        with record_path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(record, stream, indent=2, sort_keys=True, ensure_ascii=True)
            stream.write("\n")
    index = "".join(f"{entry['sha256']}  {entry['path']} bytes={entry['bytes']}\n" for entry in record["inventory"])
    index += f"{inputs.digest(record_path)}  {record_path.relative_to(args.worktree.resolve()).as_posix()} bytes={record_path.stat().st_size}\n"
    if args.verify_existing:
        require(text(index_path) == index, "frozen fingerprint index differs from actual file bytes")
    else:
        with index_path.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(index)
    print(f"NATIVE_RUNTIME_CLOSURE_PASS plain=19 controlled=35 reproductions=5 same_ctime={record['minimalReproductions']['sameCtimeAttempts']} inventory_files={len(record['inventory'])} KNOWN_LIMIT_NOT_SAFETY_PASS")
    for path in (record_path, index_path):
        print(f"CLOSURE_SHA256 {inputs.digest(path)} {path.relative_to(args.worktree.resolve()).as_posix()} bytes={path.stat().st_size}")
    for key, entry in record["elf"].items():
        print(f"NATIVE_ELF_SHA256 {entry['sha256']} {key}")


if __name__ == "__main__":
    main()

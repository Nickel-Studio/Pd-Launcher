"""Read-only linked-candidate preparation, never native startup or release acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import stat
import subprocess
import sys
import time
import zipfile

sys.dont_write_bytecode = True
HERE = Path(__file__).absolute().parent
sys.path.insert(0, str(HERE))
import delivery_smoke as smoke
import windows_runtime as runtime


def absolute_input(value: str) -> Path:
    if not isinstance(value, str) or not value:
        raise smoke.SmokeError("An exact authorized absolute input path is required")
    path = Path(value)
    if not path.is_absolute() or PureWindowsPath(value).drive.casefold() != "d:":
        raise smoke.SmokeError("Candidate input must be an explicit physical D path")
    smoke.safe_name(path.relative_to(path.anchor).as_posix())
    return smoke.plain(path, exists=False)


def observed_reference(row: dict, roots: tuple[Path, ...], *, limit: int = smoke.MAX_PACKAGE) -> dict:
    if (not isinstance(row, dict) or type(row.get("bytes")) is not int or not 0 < row["bytes"] <= limit or
            not smoke.SHA.fullmatch(str(row.get("sha256", "")))):
        raise smoke.SmokeError("Candidate reference needs bounded bytes and an independent SHA256")
    path = absolute_input(row.get("path", row.get("physicalPath")))
    if not any(path.is_relative_to(root) for root in roots):
        raise smoke.SmokeError("Candidate raw input escaped its explicitly authorized builder/evidence roots")
    if not path.exists():
        raise smoke.NotRun("Actual candidate input is absent: " + str(path))
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise smoke.SmokeError("Candidate input is not an independent regular file")
    actual = {"physicalPath": str(path), "bytes": before.st_size, "sha256": smoke.digest(path)}
    after = path.stat()
    if ((before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino) or
            any(actual[key] != row[key] for key in ("bytes", "sha256"))):
        raise smoke.SmokeError("Candidate raw input changed or differs from its independent tuple: " + str(path))
    return actual


def candidate_identity(document: dict, commit: str) -> dict:
    if (not isinstance(document, dict) or document.get("schemaVersion") != 1 or
            document.get("kind") != "builder-linked-pd-candidate" or document.get("sourceCommit") != commit or
            not smoke.COMMIT.fullmatch(commit)):
        raise smoke.SmokeError("Only the independently frozen builder-linked-pd-candidate receipt is eligible")
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, list):
        raise smoke.SmokeError("Candidate receipt has no concrete artifact inventory")
    executables = [row for row in artifacts if isinstance(row, dict) and row.get("role") == "production-executable"]
    if len(executables) != 1:
        raise smoke.SmokeError("Candidate receipt must identify exactly one production executable")
    executable = executables[0]
    if (PureWindowsPath(str(executable.get("physicalPath", ""))).name != "pd-launcher.exe" or
            executable.get("sha256") == smoke.HISTORICAL_NI_SHA):
        raise smoke.SmokeError("Historical/renamed Ni or another executable cannot be a new Pd candidate")
    return executable


def capture_selection(binding: dict, capture: dict) -> dict:
    commit = binding["sourceCommit"]
    files = capture.get("files")
    if (capture.get("kind") != "builder-candidate-committed-source-capture" or capture.get("schemaVersion") != 1 or
            capture.get("sourceCommit") != commit or capture.get("compositeSource") is not False or
            capture.get("missingInputs") != [] or not isinstance(files, list) or not 1 <= len(files) <= smoke.MAX_FILES or
            capture.get("fileCount") != len(files) or smoke.canonical(files) != binding.get("sourceSnapshotSha256") or
            capture.get("sourceSnapshotSha256") != binding.get("sourceSnapshotSha256")):
        raise smoke.SmokeError("Candidate captured-source inventory does not bind the supplied immutable generation")
    tree = smoke.git_read("rev-parse", "--verify", commit + "^{tree}").decode("ascii").strip()
    if tree != binding.get("sourceTree") or tree != capture.get("sourceTree"):
        raise smoke.SmokeError("Candidate source tree is not the independently frozen Git tree")
    entries = {}
    for token in smoke.git_read("ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not token:
            continue
        header, name = token.split(b"\t", 1)
        mode, kind, object_id = header.decode("ascii").split()
        path = name.decode("utf-8")
        if kind != "blob" or mode not in {"100644", "100755"}:
            raise smoke.SmokeError("Candidate source tree contains an unsupported nonregular Git entry")
        entries[path] = {"mode": mode, "gitObject": object_id}
    names, total = set(), 0
    for row in files:
        name = smoke.safe_name(row.get("path"))
        if name.casefold() in names or name not in entries or any(row.get(key) != value for key, value in entries[name].items()):
            raise smoke.SmokeError("Captured source path/object/mode differs from the frozen tree or collides")
        if type(row.get("bytes")) is not int or row["bytes"] < 0 or not smoke.SHA.fullmatch(str(row.get("sha256", ""))):
            raise smoke.SmokeError("Captured source lacks actual immutable blob byte identities")
        names.add(name.casefold())
        total += row["bytes"]
    if total != capture.get("fileBytes") or total > smoke.MAX_PACKAGE:
        raise smoke.SmokeError("Captured source byte sum differs from its receipt or bounded source ZIP quota")
    selected = {row["path"] for row in files}
    exclusions = capture.get("exclusions", [])
    if (not isinstance(exclusions, list) or not all(isinstance(name, str) and name in entries for name in exclusions) or
            len(set(exclusions)) != len(exclusions) or selected.intersection(exclusions)):
        raise smoke.SmokeError("Candidate explicit source exclusions are ambiguous")
    omitted = sorted(set(entries) - selected)
    return {"kind": "independent-candidate-captured-source-selection", "sourceCommit": commit, "sourceTree": tree,
            "sourceSnapshotSha256": smoke.canonical(files), "files": files, "capturedFileCount": len(files),
            "capturedFileBytes": total, "immutableTreeFileCount": len(entries), "omittedSourcePaths": omitted,
            "explicitExclusions": exclusions, "otherUnselectedSourcePaths": sorted(set(omitted) - set(exclusions)),
            "allRepositorySourcesIncluded": not omitted, "finalReleaseSourcePackage": False,
            "claim": "Exact builder-captured immutable subset only; no complete build-input or release-source claim"}


def source_zip(selection: dict, output: Path) -> dict:
    output = smoke.plain(output)
    if not output.is_relative_to(smoke.SCOPE) or not output.is_dir():
        raise smoke.SmokeError("Candidate source ZIP output is outside the exclusive owned smoke scope")
    if not smoke.COMMIT.fullmatch(str(selection.get("sourceCommit", ""))):
        raise smoke.SmokeError("Candidate source ZIP requires an exact immutable source commit")
    smoke.capacity(selection["capturedFileBytes"] + 24 * 1024 * 1024)
    path = output / ("captured-source-" + selection["sourceCommit"][:12] + ".zip")
    smoke.plain(path, exists=False)
    reader = subprocess.Popen(["git", "cat-file", "--batch"], cwd=smoke.ROOT, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 120
    try:
        with zipfile.ZipFile(path, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for index, row in enumerate(selection["files"]):
                if time.monotonic() > deadline:
                    raise smoke.SmokeError("Immutable candidate-source ZIP exceeded its bounded preparation time")
                if index % 64 == 0:
                    smoke.capacity(selection["capturedFileBytes"] + 4 * 1024 * 1024)
                reader.stdin.write((row["gitObject"] + "\n").encode("ascii"))
                reader.stdin.flush()
                header = reader.stdout.readline(256).decode("ascii").split()
                if len(header) != 3 or header[:2] != [row["gitObject"], "blob"] or int(header[2]) != row["bytes"]:
                    raise smoke.SmokeError("Git blob identity/length differs from the captured source inventory")
                member = zipfile.ZipInfo(row["path"], date_time=(1980, 1, 1, 0, 0, 0))
                member.create_system = 3
                member.external_attr = int(row["mode"], 8) << 16
                member.compress_type = zipfile.ZIP_DEFLATED
                sha, remaining = hashlib.sha256(), row["bytes"]
                with archive.open(member, "w") as target:
                    while remaining:
                        chunk = reader.stdout.read(min(remaining, 256 * 1024))
                        if not chunk:
                            raise smoke.SmokeError("Immutable source blob stream ended before its declared length")
                        sha.update(chunk)
                        target.write(chunk)
                        remaining -= len(chunk)
                if reader.stdout.read(1) != b"\n" or sha.hexdigest() != row["sha256"]:
                    raise smoke.SmokeError("Candidate source ZIP member differs from its immutable captured SHA")
        reader.stdin.close()
        if reader.wait(timeout=10):
            raise smoke.SmokeError("Owned immutable-source reader failed")
    finally:
        if reader.poll() is None:
            reader.terminate()
            reader.wait(timeout=5)
        reader.stdout.close()
        if not reader.stdin.closed:
            reader.stdin.close()
    expected = sorted(({"distributionPath": row["path"], "bytes": row["bytes"], "sha256": row["sha256"]}
                       for row in selection["files"]), key=lambda row: row["distributionPath"])
    if smoke.zip_records(path) != expected:
        raise smoke.SmokeError("Independent ZIP member hashing rejected the captured-source package")
    return {**smoke.file_ref(path), "scope": "candidate-builder-captured-source-subset", "sourceCommit": selection["sourceCommit"],
            "memberCount": len(expected), "memberInventorySha256": smoke.canonical(expected),
            "finalReleaseSourcePackage": False, "containsRuntimePrefix": False, "secondSourceRootCreated": False}


def prepare(args) -> dict:
    initial = smoke.capacity(24 * 1024 * 1024)
    with smoke.Worker():
        output = smoke.fresh_directory("linked-candidate-prepare")
        gates = {name: {"status": "NOT_RUN", "detail": detail} for name, detail in
                 (("candidateByteBinding", "Candidate bytes not yet observed"),
                  ("candidatePeImports", "Candidate static import table not yet observed"),
                  ("candidateVersionResource", "Candidate version resource not yet observed"),
                  ("capturedSourcePackage", "Same-commit captured-source subset ZIP not yet verified"),
                  ("completeBuildInputs", "Linked candidate is not a final consumed-input/resource/depfile release receipt"),
                  ("actualInstallZip", "No actual cmake --install provenance or program-runtime-only ZIP was supplied"),
                  ("nativeStartup", "Lead authorized candidate preparation only; await next explicit-roots entry EXE"),
                  ("ownedWmClose", "No native process is started during read-only candidate preparation"),
                  ("sameExeFunctionalVisual", "No final same-EXE functional/visual receipts were supplied"))}
        result = {"schemaVersion": 1, "kind": "pd-independent-linked-candidate-preparation", "createdUtc": smoke.now(),
                  "status": "NOT_RUN", "origin": "actual-read-only-builder-linked-candidate", "gates": gates,
                  "freshCapacity": initial, "completeProductVerified": False, "nativeLaunchAttempted": False,
                  "productionExecutableObserved": False, "productionExecutableBytesObserved": False,
                  "copiedExecutableBytes": 0, "copiedRuntimeBytes": 0, "secondSourceRootCreated": False,
                  "cleanWindowsHostProven": False}
        stage = "candidateByteBinding"
        try:
            receipt_path = absolute_input(args.linked_receipt)
            if not receipt_path.exists():
                raise smoke.NotRun("Actual linked candidate receipt is absent: " + str(receipt_path))
            receipt_row = {"path": str(receipt_path), "bytes": receipt_path.stat().st_size,
                           "sha256": args.expected_receipt_sha256}
            raw_receipt = observed_reference(receipt_row, (receipt_path.parent,), limit=smoke.MAX_JSON)
            binding = smoke.read_json(receipt_path)
            executable_row = candidate_identity(binding, args.expected_source_commit)
            build_root = absolute_input(binding.get("buildRoot"))
            roots = (build_root, receipt_path.parent)
            executable = absolute_input(args.expected_executable)
            if (absolute_input(executable_row.get("physicalPath")) != executable or
                    executable_row.get("sha256") != args.expected_executable_sha256 or
                    executable_row.get("bytes") != args.expected_executable_bytes):
                raise smoke.SmokeError("Builder candidate differs from the separately supplied executable tuple")
            actual_exe = observed_reference(executable_row, (build_root,))
            source_ref = observed_reference(binding.get("sourceManifest"), roots, limit=smoke.MAX_JSON)
            capture = smoke.read_json(Path(source_ref["physicalPath"]))
            result.update(sourceCommit=binding["sourceCommit"], sourceTree=binding.get("sourceTree"),
                          executableSha256=actual_exe["sha256"], sourceSnapshotSha256=binding.get("sourceSnapshotSha256"),
                          productionExecutableBytesObserved=True, candidateExecutable=actual_exe, builderRawReceipt=raw_receipt,
                          sourceCapture=source_ref, builderReportedAcceptance={key: binding.get(key) for key in
                          ("actualRuntimeAccepted", "allRootBuildComplete", "fullCTestAccepted", "releaseAccepted")})
            smoke.write_new(output / "builder-linked-candidate.raw.json", receipt_path.read_bytes())
            smoke.write_new(output / "builder-source-capture.raw.json", Path(source_ref["physicalPath"]).read_bytes())
            result["rawInputObservations"] = [observed_reference(binding[key], roots, limit=smoke.MAX_JSON) for key in
                                              ("buildInfo", "cache", "compileCommands", "linkRecipe")]
            phases = binding.get("completedProductionPhases")
            if not isinstance(phases, list) or not phases or any(row.get("returnCode") != 0 for row in phases):
                raise smoke.SmokeError("Candidate has no successful raw production link phase inventory")
            for row in phases:
                result["rawInputObservations"].extend(observed_reference(row[key], roots, limit=smoke.MAX_JSON)
                                                      for key in ("rawLog", "receipt"))
            gates[stage] = {"status": "PASS", "detail": "Actual candidate EXE/raw receipt tuple SHA observed, not release acceptance"}
            dependencies = smoke.load_dependencies()
            result["acceptanceDependencies"] = {"lock": dependencies["lock"], "files": dependencies["files"]}
            stage = "candidatePeImports"
            audit = dependencies["acceptance"].audit(executable)
            smoke.write_json(output / "candidate-pe-import-audit.json", audit)
            if (audit["sha256"] != actual_exe["sha256"] or audit["importGate"] != "passed" or audit["machine"] != "0x8664" or
                    audit["bitness"] != 64 or audit["subsystem"] != 2):
                raise smoke.SmokeError("Existing acceptance PE parser rejected the actual linked Pd Widgets candidate")
            gates[stage] = {"status": "PASS", "audit": smoke.file_ref(output / "candidate-pe-import-audit.json"),
                            "detail": "Static PE/import bytes only; live modules and clean deployment remain NOT_RUN"}
            stage = "candidateVersionResource"
            metadata = json.loads(smoke.git_read("show", binding["sourceCommit"] + ":native/metadata.json"))
            versions = runtime.version_info(executable)
            wanted = {"ProductName": "Palladium", "FileDescription": "Palladium", "InternalName": "pd-launcher",
                      "OriginalFilename": "pd-launcher.exe", "ProductVersion": metadata["version"], "FileVersion": metadata["version"]}
            if any(any(values.get(key) != value for key, value in wanted.items()) for values in versions.values()):
                raise smoke.SmokeError("Actual candidate Windows version names differ from immutable Pd metadata")
            smoke.write_json(output / "candidate-version-resource.json", versions)
            gates[stage] = {"status": "PASS", "resource": smoke.file_ref(output / "candidate-version-resource.json"),
                            "detail": "Actual version resource bytes read; no window/visual claim"}
            stage = "capturedSourcePackage"
            selection = capture_selection(binding, capture)
            smoke.write_json(output / "source-selection.json", selection)
            result["sourceSelection"] = smoke.file_ref(output / "source-selection.json")
            result["capturedSourcePackage"] = source_zip(selection, output)
            gates[stage] = {"status": "PASS", "capturedFileCount": selection["capturedFileCount"],
                            "omittedRepositoryFileCount": len(selection["omittedSourcePaths"]),
                            "detail": "Same-commit immutable captured subset ZIP verified; not final release source closure"}
            observed_reference(executable_row, (build_root,))
            observed_reference(receipt_row, (receipt_path.parent,), limit=smoke.MAX_JSON)
            observed_reference(binding["sourceManifest"], roots, limit=smoke.MAX_JSON)
            result["finalCapacity"] = smoke.capacity()
        except smoke.NotRun as error:
            gates[stage] = {"status": "NOT_RUN", "detail": str(error)}
        except (OSError, ValueError, KeyError, TypeError, ImportError, subprocess.SubprocessError) as error:
            gates[stage] = {"status": "FAIL", "detail": str(error)}
        result["status"] = smoke.status(gates)
        result["completedUtc"] = smoke.now()
        smoke.write_json(output / "candidate-prepare-receipt.json", result)
        return {"status": result["status"], "receipt": smoke.file_ref(output / "candidate-prepare-receipt.json"),
                "nativeLaunchAttempted": False, "gates": gates}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linked-receipt", required=True)
    parser.add_argument("--expected-receipt-sha256", required=True)
    parser.add_argument("--expected-source-commit", required=True)
    parser.add_argument("--expected-executable", required=True)
    parser.add_argument("--expected-executable-sha256", required=True)
    parser.add_argument("--expected-executable-bytes", required=True, type=int)
    args = parser.parse_args()
    try:
        result = prepare(args)
    except smoke.NotRun as error:
        result = {"status": "NOT_RUN", "receipt": None, "nativeLaunchAttempted": False, "detail": str(error)}
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        result = {"status": "FAIL", "receipt": None, "nativeLaunchAttempted": False, "detail": str(error)}
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return {"PASS": 0, "FAIL": 1, "NOT_RUN": 2}[result["status"]]


if __name__ == "__main__":
    raise SystemExit(main())

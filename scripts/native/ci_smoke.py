"""Prepare and publish the existing same-graph smoke protocol for native CI."""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

from ci_evidence import (CLIENT, EvidenceError, api_child, checked_command, cmake_application,
                         file_ref, inside, read_json, write_json)

ROOT = Path(__file__).resolve().parents[2]
TARGET = "nickel_production_smoke_existing_matrix"
SOURCE_ROOTS = ("native", "cmake", "scripts/native", "tests/native-acceptance", "tests/native-production-smoke",
                "tests/brand", "tests/native-release", "tests/native-portability", "src/assets", "LICENSES")
SOURCE_FILES = {".gitattributes", ".gitignore", "CMakeLists.txt", "CMakePresets.json", "CHANGELOG.json",
                "LICENSE", "LICENSE-ASSETS"}


def canonical_bytes(value) -> bytes:
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def digest(value) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def target_platform(value: str | None = None) -> str:
    native = {"win32": "windows", "linux": "linux"}.get(sys.platform)
    if native is None or value not in (None, native):
        raise EvidenceError("CI smoke inputs must bind the actual native Windows or Linux target")
    return native


def selected_source(name: str) -> bool:
    path = PurePosixPath(name)
    if name not in SOURCE_FILES and not any(name == root or name.startswith(root + "/") for root in SOURCE_ROOTS):
        return False
    if any(part in {"CMakeFiles", ".deps", ".build-policy", "scratch", "history", "_local", "testsScratch6"}
           or part.startswith((".build-", ".scratch")) for part in path.parts):
        return False
    return path.name not in {"CMakeCache.txt", "compile_commands.json", "install_manifest.txt"} and path.suffix not in {
        ".exe", ".obj", ".lib", ".map", ".pdb", ".ninja", ".log"}


def capture_sources(root: Path, commit: str) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise EvidenceError("CI source commit must be a full lowercase Git commit")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root).decode().strip()
    if actual != commit or subprocess.check_output(["git", "status", "--porcelain=v1", "--untracked-files=no"], cwd=root).strip():
        raise EvidenceError("CI must inventory the clean event-bound checkout")
    tree = subprocess.check_output(["git", "ls-tree", "-r", "-z", "--long", commit], cwd=root)
    entries = []
    for entry in tree.split(b"\0"):
        if not entry:
            continue
        info, raw_name = entry.split(b"\t", 1)
        mode, kind, obj, size = info.decode("ascii").split()
        name = raw_name.decode("utf-8")
        if not selected_source(name):
            continue
        if kind != "blob" or mode not in {"100644", "100755"}:
            raise EvidenceError("CI source inventory contains a nonregular input")
        entries.append({"path": name, "mode": mode, "gitObject": obj, "bytes": int(size)})
    committed, files = [], []
    process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        for entry in sorted(entries, key=lambda row: row["path"]):
            process.stdin.write((entry["gitObject"] + "\n").encode("ascii"))
            process.stdin.flush()
            header = process.stdout.readline().decode("ascii").strip().split()
            if header != [entry["gitObject"], "blob", str(entry["bytes"])]:
                raise EvidenceError("Git blob inventory and actual source object differ")
            blob = process.stdout.read(entry["bytes"])
            if len(blob) != entry["bytes"] or process.stdout.read(1) != b"\n":
                raise EvidenceError("Truncated committed source blob")
            committed.append({**entry, "sha256": hashlib.sha256(blob).hexdigest()})
            actual_path = inside(root / entry["path"], root)
            if actual_path.is_symlink() or not actual_path.is_file():
                raise EvidenceError("Actual build source is not a regular checkout file")
            actual_ref = file_ref(actual_path)
            files.append({**entry, "sha256": actual_ref["sha256"], "bytes": actual_ref["bytes"],
                          "gitBlobSha256": committed[-1]["sha256"], "gitBlobBytes": entry["bytes"]})
    finally:
        process.stdin.close()
        process.stdout.close()
        if process.wait(timeout=30):
            raise EvidenceError("Committed source capture failed")
    if not files:
        raise EvidenceError("CI actual source inventory cannot be empty")
    return {"schemaVersion": 1, "kind": "builder-candidate-ci-checkout-source-capture", "sourceCommit": commit,
            "sourceTree": subprocess.check_output(["git", "rev-parse", commit + "^{tree}"], cwd=root).decode().strip(),
            "sourceRoot": str(root), "files": files, "committedFiles": committed,
            "sourceSnapshotSha256": digest(committed), "actualSourceInventorySha256": digest(files),
            "fileCount": len(files), "fileBytes": sum(row["bytes"] for row in files),
            "checkoutByteDifferences": [row["path"] for row in files if (row["sha256"], row["bytes"]) !=
                                        (row["gitBlobSha256"], row["gitBlobBytes"])],
            "completeReleaseBuildInputs": False, "releaseAccepted": False}


def smoke_tools(root: Path):
    location = root / "tests/native-production-smoke"
    if not (location / "prepare_same_graph.py").is_file():
        raise EvidenceError("The committed same-graph owner tools are missing")
    sys.path.insert(0, str(location))
    generator = importlib.import_module("generate_fixtures")
    verifier = importlib.import_module("prepare_same_graph")
    if Path(generator.__file__).resolve().parent != location.resolve() or Path(verifier.__file__).resolve().parent != location.resolve():
        raise EvidenceError("Smoke tools were imported from a different source tree")
    return generator, verifier


def prepare(root: Path, output: Path, commit: str, platform: str | None = None) -> dict:
    root = root.resolve()
    output = inside(output, root)
    platform = target_platform(platform)
    namespace = root / ".snow/production-smoke/ci"
    if namespace.exists() or output.exists():
        raise EvidenceError("CI smoke preparation needs fresh output paths")
    sources = capture_sources(root, commit)
    source_path = namespace / "source-manifest.json"
    write_json(source_path, sources)
    harness_files = [{"path": row["path"], "size": row["bytes"], "sha256": row["sha256"]}
                     for row in sources["files"] if row["path"].startswith("tests/native-production-smoke/")]
    required = {"production_main.cpp", "suite_cases.hpp", "qt_probe.hpp", "same_graph_inputs.hpp", "CMakeLists.txt", "matrix.json"}
    if not required.issubset({PurePosixPath(row["path"]).name for row in harness_files}):
        raise EvidenceError("Committed harness omits an actual compiled matrix input")
    harness_path = namespace / "harness-manifest.json"
    write_json(harness_path, {"schema": 1, "kind": "owned-production-smoke-source-delta", "owner": "production-smoke",
                             "sourceCommit": commit, "files": harness_files, "productionPass": False})
    generator, _ = smoke_tools(root)
    metadata = read_json(root / "native/metadata.json")
    source_pin, harness_pin = file_ref(source_path)["sha256"], file_ref(harness_path)["sha256"]
    fixtures = generator.generate(root / ".snow/production-smoke/fixtures/ci", metadata["version"], deep=True,
                                  source_commit=commit, source_sha256=source_pin, harness_sha256=harness_pin)
    fixture = Path(fixtures["root"]) / "fixture-manifest.json"
    plan = {"schemaVersion": 1, "kind": "native-ci-same-graph-smoke-plan", "sourceCommit": commit,
            "sourceRoot": str(root), "targetPlatform": platform,
            "sourceManifest": file_ref(source_path), "harnessManifest": file_ref(harness_path),
            "fixtureManifest": file_ref(fixture), "harnessRoot": str(root), "ownerRoot": str(root),
            "matrix": str(root / "tests/native-production-smoke/matrix.json"),
            "verifiedFreeze": str(namespace / "verified-inputs.json"), "caseOutput": str(namespace / "cases.json"),
            "releaseAccepted": False}
    arguments = {"SAME_GRAPH": "ON", "REGISTER_TEST": "ON", "SOURCE_COMMIT": commit,
                 "SOURCE_SHA256": source_pin, "HARNESS_SHA256": harness_pin, "HARNESS_ROOT": root.as_posix(),
                 "OWNER_ROOT": root.as_posix(), "VERIFIED_FREEZE": Path(plan["verifiedFreeze"]).as_posix(),
                 "FIXTURE_MANIFEST": fixture.as_posix(), "CASE_OUTPUT": Path(plan["caseOutput"]).as_posix()}
    plan["configureArguments"] = ["-DNICKEL_PRODUCTION_SMOKE_" + key + "=" + str(value) for key, value in arguments.items()]
    write_json(output, plan)
    return plan


def target_artifact(build: Path, target: dict) -> Path:
    name = target.get("nameOnDisk")
    matches = [inside(build / row["path"], build) for row in target.get("artifacts", [])
               if Path(row["path"]).name == name]
    if len(matches) != 1 or not matches[0].is_file():
        raise EvidenceError("The actual production target has no unique existing artifact")
    return matches[0]


def codemodel_targets(build: Path) -> dict:
    reply = build / ".cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise EvidenceError("The current build has no actual File API reply")
    index = read_json(indexes[-1])
    model = read_json(api_child(reply, index["reply"][CLIENT]["codemodel-v2"]["jsonFile"]))
    configurations = [row for row in model["configurations"] if row.get("name") == "Release"]
    if len(configurations) != 1 or Path(model["paths"]["build"]).resolve() != build:
        raise EvidenceError("The actual Release graph differs from the declared build")
    return {row["id"]: read_json(api_child(reply, row["jsonFile"])) for row in configurations[0]["targets"]}


def dependency_closure(targets: dict, start: str) -> set[str]:
    names = [key for key, target in targets.items() if target.get("name") == start]
    if len(names) != 1:
        raise EvidenceError("The actual graph omits or duplicates the required consumer")
    pending, visited = names[:], set()
    while pending:
        key = pending.pop()
        if key in visited:
            continue
        visited.add(key)
        pending.extend(row["id"] for row in targets[key].get("dependencies", []) if row["id"] in targets)
    return visited


def publish(root: Path, build: Path, plan_path: Path, inventory_path: Path, build_evidence: Path) -> dict:
    root, build = root.resolve(), inside(build, root)
    plan = read_json(inside(plan_path, root))
    if plan.get("kind") != "native-ci-same-graph-smoke-plan" or Path(plan["sourceRoot"]) != root:
        raise EvidenceError("CI smoke publication uses a different source plan")
    platform = target_platform(plan.get("targetPlatform"))
    if Path(plan["harnessRoot"]) != root or Path(plan["ownerRoot"]) != root:
        raise EvidenceError("CI must use its own committed harness and owner tools")
    checked_command(inside(build_evidence, root), "cmake", ("--build",))
    for key in ("sourceManifest", "harnessManifest", "fixtureManifest"):
        if file_ref(inside(Path(plan[key]["path"]), root)) != plan[key]:
            raise EvidenceError("Prepared same-graph input changed before the actual build completed")
    source = read_json(Path(plan["sourceManifest"]["path"]))
    if source.get("sourceCommit") != plan["sourceCommit"] or not source.get("files"):
        raise EvidenceError("Prepared raw source inventory does not bind the actual source commit")
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root).decode().strip() != plan["sourceCommit"]:
        raise EvidenceError("CI source commit changed after configuration")
    for row in source["files"]:
        observed = file_ref(inside(root / row["path"], root))
        if (observed["bytes"], observed["sha256"]) != (row["bytes"], row["sha256"]):
            raise EvidenceError("An actual compiler/resource source changed during CI")
    application = cmake_application(build, root)
    targets = codemodel_targets(build)
    closure = dependency_closure(targets, TARGET)
    if not any(targets[key].get("name") == "nickel_app_services" for key in closure):
        raise EvidenceError("The existing matrix is not linked to the real AppServices target")
    smoke_targets = [row for row in targets.values() if row.get("name") == TARGET and row.get("type") == "EXECUTABLE"]
    if len(smoke_targets) != 1:
        raise EvidenceError("The actual smoke executable is missing")
    artifacts = []
    for key in sorted(closure):
        target = targets[key]
        if target.get("type") == "STATIC_LIBRARY":
            artifacts.append({"role": "production-library", **file_ref(target_artifact(build, target))})
    artifacts += [{"role": "production-executable", **file_ref(Path(application["artifact"]))},
                  {"role": "smoke-executable", **file_ref(target_artifact(build, smoke_targets[0]))}]
    for row in artifacts:
        row["physicalPath"] = row.pop("path")
    source_pin, harness_pin = plan["sourceManifest"]["sha256"], plan["harnessManifest"]["sha256"]
    compile_path, link_path = build / "compile_commands.json", build / "build.ninja"
    namespace = inside(Path(plan["verifiedFreeze"]).parent, root)
    identities = {"sourceCommit": plan["sourceCommit"], "sourceGenerationManifestSha256": source_pin,
                  "harnessManifestSha256": harness_pin, "targetPlatform": platform}
    ledger_path = namespace / "artifact-manifest.json"
    write_json(ledger_path, {"kind": "matched-same-graph-production-artifacts", **identities, "artifacts": artifacts})
    complete_path = namespace / "publication-complete.json"
    write_json(complete_path, {"publicationComplete": True, "receiver": "production-smoke", **identities,
                              "artifactManifestSha256": file_ref(ledger_path)["sha256"],
                              "compileCommandsSha256": file_ref(compile_path)["sha256"], "linkRecipeSha256": file_ref(link_path)["sha256"],
                              "actualBuildCommand": file_ref(build_evidence), "runtimeAccepted": False, "releaseAccepted": False})
    _, verifier = smoke_tools(root)
    # Only the complete owner verifier may publish the consumer's verified freeze.
    args = argparse.Namespace(source_manifest=plan["sourceManifest"]["path"], source_sha256=source_pin,
        source_commit=plan["sourceCommit"], harness_manifest=plan["harnessManifest"]["path"], harness_sha256=harness_pin,
        harness_root=str(root), artifact_manifest=str(ledger_path), artifact_sha256=file_ref(ledger_path)["sha256"],
        complete_marker=str(complete_path), complete_sha256=file_ref(complete_path)["sha256"],
        compile_commands=str(compile_path), compile_sha256=file_ref(compile_path)["sha256"],
        link_recipe=str(link_path), link_sha256=file_ref(link_path)["sha256"],
        ctest_inventory=str(inside(inventory_path, root)), ctest_sha256=file_ref(inventory_path)["sha256"],
        fixtures=plan["fixtureManifest"]["path"], case_output=str(inside(Path(plan["caseOutput"]), root)),
        output=str(inside(Path(plan["verifiedFreeze"]), root)), target_platform=platform)
    return verifier.prepare(args)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--output", type=Path, required=True)
    prep.add_argument("--source-commit", required=True)
    prep.add_argument("--target-platform", choices=("windows", "linux"))
    pub = commands.add_parser("publish")
    for name in ("build", "plan", "inventory", "build-evidence"):
        pub.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(ROOT, args.output, args.source_commit, args.target_platform) if args.action == "prepare" else publish(ROOT, args.build, args.plan, args.inventory, args.build_evidence)
        print(json.dumps({"kind": result["kind"], "sourceCommit": result["sourceCommit"], "runtimeAccepted": False}))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print("CI smoke BLOCKED:", str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

"""Collect final launcher release gates without reimplementing the acceptance PE parser."""
from __future__ import annotations

import argparse
from collections import Counter
from functools import lru_cache
from pathlib import Path, PureWindowsPath
import re
import sys

from evidence_common import (ROOT, EvidenceError, MissingEvidence, actual_commit, canonical_sha, copy_small_input,
                             exit_code, file_ref, gate, inside, load_python_tool, new_output,
                             overall, read_git, read_json, reference, relative_name, sha_file, utc_now,
                             write_json)

RUNTIME_ROLES = {"application", "runtime-dependency", "embedded-notice", "data-resource"}
MATERIAL_ROLES = {"corresponding-source", "application-objects", "relink-material", "sbom",
                  "license-material", "build-log", "link-command", "link-map", "dependency-inventory",
                  "build-source-inventory", "build-input-consumption", "symbols", "installation-information"}
REQUIRED_PLATFORMS = ("windows-x64", "linux-x64", "macos-arm64", "macos-x64")
FUNCTION_IDS = {"A01", "D01", "D02", "D03", "D04", "N01", "N02", "N03", "M01", "M02", "M03",
                "I01", "I02", "R01", "R02", "R03", "P01", "P02", "P03", "L01", "U01", "U02", "U03", "O01"}
HISTORICAL_PRODUCTS = frozenset({"Nickel", "ni-launcher"})
PALLADIUM_DISPLAY_NAMES = frozenset({"Palladium", "Pd", "\u94af"})
PALLADIUM_EXECUTABLE_NAME = "pd-launcher"
PALLADIUM_PRODUCTS = PALLADIUM_DISPLAY_NAMES | frozenset({PALLADIUM_EXECUTABLE_NAME})


def canonical_product(value: object) -> str:
    if not isinstance(value, str) or value not in HISTORICAL_PRODUCTS | PALLADIUM_PRODUCTS:
        raise EvidenceError("Artifact product is not a recognized historical Nickel or current Palladium identity")
    return "Palladium" if value in PALLADIUM_PRODUCTS else "Nickel"


@lru_cache(maxsize=4)
def _mapped_source_snapshot(root: Path, original: str, source: str, source_tree: str, source_parent: str) -> bytes:
    from source_audit import source_snapshot, tree_entries
    for value in (original, source, source_parent):
        try:
            resolved = read_git(root, "--no-replace-objects", "rev-parse", "--verify", value + "^{commit}").decode("ascii").strip()
        except EvidenceError as error:
            raise MissingEvidence(f"Snapshot mapping needs independently readable immutable commit {value}") from error
        if resolved != value:
            raise EvidenceError("Snapshot mapping commit did not resolve to the exact supplied object")
    trees = [read_git(root, "--no-replace-objects", "rev-parse", value + "^{tree}").decode("ascii").strip()
             for value in (original, source)]
    if trees != [source_tree, source_tree]:
        raise EvidenceError("Original and CI source commits do not reproduce the claimed same Git tree")
    header = read_git(root, "--no-replace-objects", "cat-file", "-p", source).split(b"\n\n", 1)[0]
    parents = [line[7:].decode("ascii") for line in header.splitlines() if line.startswith(b"parent ")]
    if parents != [source_parent]:
        raise EvidenceError("CI snapshot commit does not have the exact declared safe parent")
    if tree_entries(root, original) != tree_entries(root, source):
        raise EvidenceError("Original and CI source tree records differ")
    from evidence_common import json_bytes
    return json_bytes(source_snapshot(root, source))


def verify_snapshot_mapping(root: Path, artifact: dict) -> dict | None:
    fields = {"originalSourceCommit", "sourceTree", "snapshotMapping"}
    if not fields.intersection(artifact):
        return None
    if artifact.get("kind") != "native-release" or canonical_product(artifact.get("product")) != "Palladium":
        raise EvidenceError("Only current Palladium source identity may carry a CI snapshot mapping")
    if fields - set(artifact):
        raise MissingEvidence("Original/CI source identity needs originalSourceCommit, sourceTree and hash-referenced snapshotMapping")
    original, source, tree = artifact["originalSourceCommit"], artifact["sourceCommit"], artifact["sourceTree"]
    for value in (original, source, tree):
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", value):
            raise EvidenceError("Snapshot mapping commit/tree identities must be exact full Git object IDs")
    if original == source:
        raise EvidenceError("Snapshot mapping must explicitly distinguish original and actual CI source commits")
    path = reference(root, artifact["snapshotMapping"])
    document = read_json(path)
    if (not isinstance(document, dict) or document.get("schemaVersion") != 1 or
            document.get("kind") != "actual-git-source-snapshot-mapping" or
            document.get("origin") != "actual-git-commit-tree-and-blob-verification"):
        raise EvidenceError("Claimed same-SHA reports are not an actual Git source snapshot mapping")
    expected = {"originalSourceCommit": original, "sourceCommit": source, "sourceTree": tree,
                "sourceSnapshotSha256": artifact["sourceSnapshotSha256"]}
    if any(document.get(key) != value for key, value in expected.items()):
        raise EvidenceError("Snapshot mapping does not bind the exact original/CI source generation")
    raw = document.get("rawEvidence")
    if not isinstance(raw, list) or not raw:
        raise MissingEvidence("Original/CI snapshot mapping needs actual raw tree/ref evidence")
    for row in raw:
        reference(root, row)
    files = document.get("sourceFiles")
    if not isinstance(files, list) or not 1 <= len(files) <= 20000 or type(document.get("fileCount")) is not int or document["fileCount"] != len(files):
        raise EvidenceError("Snapshot mapping needs a complete bounded committed source inventory")
    if files != sorted(files, key=lambda row: row["path"]) or canonical_sha(files) != artifact["sourceSnapshotSha256"]:
        raise EvidenceError("Snapshot mapping source records do not reproduce the declared canonical snapshot")
    parent = document.get("sourceParentCommit")
    if not isinstance(parent, str) or not re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", parent):
        raise EvidenceError("CI snapshot mapping needs the exact public-safe parent commit")
    from evidence_common import json_bytes
    if json_bytes(files) != _mapped_source_snapshot(root.resolve(), original, source, tree, parent):
        raise EvidenceError("Snapshot mapping differs from actual committed tree/blob bytes; rehashing a claim is insufficient")
    return {"mapping": file_ref(path, root), **expected, "sourceParentCommit": parent, "fileCount": len(files),
            "sourceFiles": files, "verifiedActualGitBlobs": True}


def same_source_generation(root: Path, artifact: dict, candidate: dict) -> bool:
    mappings = [(verify_snapshot_mapping(root, mapped), other) for mapped, other in
                ((artifact, candidate), (candidate, artifact))]
    if artifact.get("sourceSnapshotSha256") != candidate.get("sourceSnapshotSha256"):
        return False
    if artifact.get("sourceCommit") == candidate.get("sourceCommit"):
        return True
    return any(mapping and other.get("sourceCommit") in {mapping["originalSourceCommit"], mapping["sourceCommit"]}
               for mapping, other in mappings)


def bind_native_generation(document: dict, artifact: dict, label: str) -> None:
    if artifact.get("kind") != "native-release" or canonical_product(artifact.get("product")) != "Palladium":
        return
    if not isinstance(document, dict):
        raise EvidenceError(f"{label} must be an actual same-generation evidence object")
    expected = {"product": artifact["product"], "sourceCommit": artifact["sourceCommit"],
                "sourceSnapshotSha256": artifact["sourceSnapshotSha256"],
                "buildSourceSha256": artifact["buildSourceSha256"], "executableSha256": artifact["executable"]["sha256"]}
    for key, value in expected.items():
        if key not in document:
            raise MissingEvidence(f"{label} lacks current Palladium {key}; historical evidence cannot be relabeled")
        if document[key] != value:
            raise EvidenceError(f"{label} identifies a different Palladium {key} or build generation")


def verify_product_identity(root: Path, artifact: dict, files: list[dict]) -> dict | None:
    if canonical_product(artifact.get("product")) != "Palladium":
        return None
    row = artifact.get("productIdentityInput")
    path = reference(root, row)
    source_path = relative_name(row.get("sourcePath"))
    consumed = [item for item in files if all(item.get(key) == row.get(key) for key in ("path", "sha256", "bytes"))]
    if len(consumed) != 1 or consumed[0]["role"] not in {"resource-input", "build-configuration", "generated-input"}:
        raise EvidenceError("Palladium product identity must be one actual consumed metadata input, not a renamed sidecar")
    metadata = read_json(path)
    if not isinstance(metadata, dict) or metadata.get("schemaVersion") != 1:
        raise EvidenceError("Consumed native product metadata must have schema version 1")
    display = metadata.get("displayName")
    if not isinstance(display, str) or display not in PALLADIUM_DISPLAY_NAMES:
        raise EvidenceError("Actual consumed product metadata is historical Nickel or not Palladium; relabeling reports is insufficient")
    for field in ("name", "executableName"):
        if metadata.get(field) != PALLADIUM_EXECUTABLE_NAME:
            raise EvidenceError(f"Consumed Palladium metadata {field} must identify pd-launcher, not a historical or renamed executable")
    if metadata.get("version") != artifact["version"]:
        raise EvidenceError("Consumed Palladium metadata version differs from the actual Artifact")
    if artifact["platform"] == "windows":
        expected_name = metadata["executableName"] + ".exe"
        applications = [item for item in artifact["files"] if item.get("role") == "application" and
                        item.get("sha256") == artifact["executable"]["sha256"]]
        if len(applications) != 1 or Path(relative_name(applications[0].get("distributionPath"))).name.casefold() != expected_name.casefold():
            raise EvidenceError("Palladium Windows EXE deployment name differs from the actual consumed executableName")
    return {"input": file_ref(path, root), "sourcePath": source_path, "sourceArchivePath": consumed[0]["sourceArchivePath"],
            "canonicalProduct": "Palladium", "name": metadata["name"], "displayName": display,
            "executableName": metadata["executableName"], "version": metadata["version"]}


def verify_build_inputs(root: Path, artifact: dict) -> dict:
    digest = artifact.get("buildSourceSha256")
    if digest is None:
        raise MissingEvidence("Actual final native build-source SHA-256 is missing; a committed snapshot is insufficient")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise EvidenceError("Actual build-source SHA-256 is malformed")
    manifest_path = reference(root, artifact.get("buildSourceInventory"))
    document = read_json(manifest_path)
    if (not isinstance(document, dict) or document.get("schemaVersion") != 1 or
            document.get("kind") != "actual-native-build-source-inventory" or document.get("scope") != "full-launcher"):
        raise EvidenceError("Build inputs must identify the actual full native build, not installed-prefix or committed-only inventory")
    bind_native_generation(document, artifact, "Build-source inventory")
    identity = {"sourceCommit": artifact["sourceCommit"], "sourceSnapshotSha256": artifact["sourceSnapshotSha256"],
                "executableSha256": artifact["executable"]["sha256"], "configuration": "Release", "buildSourceSha256": digest}
    if any(document.get(key) != value for key, value in identity.items()):
        raise EvidenceError("Build-source inventory identifies a different executable, source snapshot or Release build")
    if document.get("fileSetComplete") is not True:
        raise MissingEvidence("Actual compiled/generated/resource build-input closure has not been completed")
    files = document.get("files")
    if not isinstance(files, list) or not 1 <= len(files) <= 20000:
        raise EvidenceError("Actual build inputs need a nonempty bounded file inventory")
    if type(document.get("fileCount")) is not int or document["fileCount"] != len(files):
        raise EvidenceError("Build-source inventory count differs from its actual records")
    keys = {"path", "sourceArchivePath", "role", "sha256", "bytes"}
    roles = {"compiled-source", "included-header", "generated-input", "resource-input", "build-configuration"}
    names, archive_names, observed_roles = set(), set(), set()
    for row in files:
        if not isinstance(row, dict) or set(row) != keys or row.get("role") not in roles:
            raise EvidenceError("Every actual build input needs an exact path/role/hash/bytes/sourceArchivePath record")
        name, member = relative_name(row["path"]), relative_name(row["sourceArchivePath"])
        if name != row["path"] or member != row["sourceArchivePath"] or name.casefold() in names or member.casefold() in archive_names:
            raise EvidenceError("Build input or corresponding-source member paths are ambiguous or collide")
        if type(row["bytes"]) is not int or row["bytes"] < 1:
            raise EvidenceError("Build-input byte lengths must be positive actual integers")
        reference(root, row)
        names.add(name.casefold())
        archive_names.add(member.casefold())
        observed_roles.add(row["role"])
    if files != sorted(files, key=lambda row: row["path"]) or canonical_sha(files) != digest:
        raise EvidenceError("Actual sorted build-input records do not match buildSourceSha256")
    if {"compiled-source", "generated-input", "resource-input", "build-configuration"} - observed_roles:
        raise MissingEvidence("Native Qt Release input inventory omits compiled, generated, resource or build-configuration inputs")
    consumption_path = reference(root, document.get("consumptionEvidence"))
    consumption = read_json(consumption_path)
    if (not isinstance(consumption, dict) or consumption.get("schemaVersion") != 1 or
            consumption.get("kind") != "actual-native-build-input-consumption" or
            consumption.get("origin") != "actual-build-commands-depfiles-and-resource-compilation"):
        raise EvidenceError("A source listing alone is not actual compiler/generator/resource consumption evidence")
    bind_native_generation(consumption, artifact, "Build-input consumption")
    if any(consumption.get(key) != value for key, value in identity.items()) or consumption.get("files") != files:
        raise EvidenceError("Actual build consumption evidence does not bind the same final inputs and executable")
    if any(consumption.get(key) is not True for key in ("compilerDependencyClosureComplete", "generatedInputsComplete", "resourceInputsComplete")):
        raise MissingEvidence("Actual compiler dependency/generated/resource consumption closure is incomplete")
    raw = consumption.get("rawEvidence")
    if not isinstance(raw, list) or not raw:
        raise MissingEvidence("Actual build input consumption has no raw commands/depfiles/resource evidence")
    for row in raw:
        reference(root, row)
    build = artifact.get("buildEvidence", {})
    for role in ("build-log", "link-command"):
        if build.get(role) not in raw:
            raise EvidenceError("Input consumption must include the same actual build log and final link command as the Artifact")
    source_archive = document.get("correspondingSourceArchive")
    archive_path = reference(root, source_archive)
    if source_archive.get("format") not in {"zip", "tar", "tar.gz", "tar.xz"}:
        raise EvidenceError("Actual corresponding source must be an independently readable ZIP/TAR archive")
    expected_delivery = ((artifact["buildSourceInventory"], "build-source-inventory"),
                         (document["consumptionEvidence"], "build-input-consumption"),
                         (source_archive, "corresponding-source"))
    for material, role in expected_delivery:
        if sum(row.get("role") == role and all(row.get(key) == material[key] for key in ("path", "sha256", "bytes"))
               for row in artifact["files"]) != 1:
            raise EvidenceError("Artifact must deliver the exact build inventory, consumption evidence and corresponding source once each")
    from archive_inventory import inventory
    members = inventory(archive_path, source_archive["format"])
    by_name = {row["distributionPath"]: row for row in members}
    for row in files:
        member = by_name.get(row["sourceArchivePath"])
        if member is None or any(member[key] != row[key] for key in ("sha256", "bytes")):
            raise EvidenceError("Corresponding source archive does not reproduce an actual compiled/generated/resource input")
    product_identity = verify_product_identity(root, artifact, files)
    return {"buildSourceSha256": digest, "inventory": file_ref(manifest_path, root), "fileCount": len(files),
            "consumptionEvidence": file_ref(consumption_path, root), "correspondingSourceArchive": file_ref(archive_path, root),
            "archiveMemberCount": len(members), "archiveMembersSha256": canonical_sha(members), "productIdentity": product_identity,
            "limitations": ["Hashes and source-archive correspondence do not alone prove feature equivalence or independently rerun the compiler.",
                            "Actual raw command/dependency/resource evidence remains inspectable; prefix/component inventories do not substitute."]}


def verify_manifest(root: Path, document: dict, *, expected_kind: str | None = None,
                    require_current_native: bool = False) -> dict:
    if not isinstance(document, dict) or document.get("schemaVersion") != 1 or document.get("scope") != "full-launcher":
        raise EvidenceError("Only a complete full-launcher Artifact manifest can satisfy release evidence")
    kind = document.get("kind")
    if kind not in {"legacy-release", "native-release"} or expected_kind and kind != expected_kind:
        raise EvidenceError("Artifact must explicitly identify legacy-release or native-release, never components/probes")
    if document.get("configuration") != "Release":
        raise EvidenceError("Artifact must identify an actual Release build")
    product = canonical_product(document.get("product"))
    if kind == "legacy-release" and product != "Nickel":
        raise EvidenceError("Frozen legacy Release identity must remain Nickel; Palladium cannot replace the legacy oracle")
    if require_current_native:
        if kind != "native-release":
            raise EvidenceError("Current native validation cannot be applied to a legacy Release")
        if product != "Palladium":
            raise MissingEvidence("Historical native Nickel is inspectable but is not current Palladium Release evidence")
    for field in ("releaseId", "version", "platform", "architecture", "sourceCommit"):
        if not isinstance(document.get(field), str) or not document[field]:
            raise EvidenceError(f"Artifact manifest lacks {field}")
    if document["platform"] not in {"windows", "linux", "macos"} or document["architecture"] not in {"x64", "arm64"}:
        raise EvidenceError("Unsupported release platform/architecture identity")
    if not re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", document["sourceCommit"]):
        raise EvidenceError("Artifact sourceCommit must identify an exact actual source commit")
    if kind == "native-release" and not re.fullmatch(r"[0-9a-f]{64}", str(document.get("sourceSnapshotSha256", ""))):
        raise MissingEvidence("Final native source SHA-256 snapshot has not been supplied")
    if kind == "native-release" and product == "Palladium":
        digest = document.get("buildSourceSha256")
        if digest is None:
            raise MissingEvidence("Current Palladium actual build-source SHA-256 has not been supplied")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise EvidenceError("Current Palladium actual build-source SHA-256 is malformed")
    source_mapping = verify_snapshot_mapping(root, document)
    executable = reference(root, document.get("executable"))
    provenance_path = reference(root, document.get("releaseProvenance"))
    provenance = read_json(provenance_path)
    if (not isinstance(provenance, dict) or provenance.get("kind") != "actual-release-provenance" or
            provenance.get("sourceCommit") != document["sourceCommit"] or provenance.get("configuration") != "Release" or
            provenance.get("executableSha256") != document["executable"]["sha256"]):
        raise EvidenceError("Actual published/build Release provenance does not match the executable/source")
    bind_native_generation(provenance, document, "Release provenance")
    if provenance.get("origin") not in {"published-release", "actual-source-build"} or not provenance.get("rawEvidence"):
        raise MissingEvidence("Release origin has no published metadata/build raw evidence")
    for row in provenance["rawEvidence"]:
        reference(root, row)
    files = document.get("files")
    if not isinstance(files, list) or not files:
        raise MissingEvidence("Complete Artifact file manifest is required")
    names, runtime, ancillary, roles = set(), [], [], Counter()
    for row in files:
        name = relative_name(row.get("distributionPath", ""))
        if name.casefold() in names:
            raise EvidenceError("Duplicate/case-colliding Artifact distribution path")
        names.add(name.casefold())
        path = reference(root, row)
        role = row.get("role")
        if role not in RUNTIME_ROLES | MATERIAL_ROLES:
            raise EvidenceError(f"Artifact role is missing or not an explicit scope: {name}")
        roles[role] += 1
        measured = {**row, "bytes": path.stat().st_size}
        (runtime if role in RUNTIME_ROLES else ancillary).append(measured)
    if sum(row["role"] == "application" and row["sha256"] == document["executable"]["sha256"] for row in runtime) != 1:
        raise EvidenceError("The exact final executable must occur once in the runtime manifest")
    deployment = document.get("deployment")
    if not isinstance(deployment, dict) or deployment.get("fileSetComplete") is not True:
        raise MissingEvidence("Complete program deployment file set has not been verified")
    if not isinstance(deployment.get("externalRequirements"), list):
        raise MissingEvidence("Installed/shared dependencies and minimum platform must be disclosed separately")
    closure_path = reference(root, deployment.get("runtimeClosureEvidence"))
    closure = read_json(closure_path)
    expected_runtime = sorted(({key: row[key] for key in ("distributionPath", "sha256", "bytes")} for row in runtime),
                              key=lambda row: row["distributionPath"])
    if (not isinstance(closure, dict) or closure.get("kind") != "actual-program-runtime-closure" or
            closure.get("executableSha256") != document["executable"]["sha256"] or closure.get("files") != expected_runtime or
            closure.get("externalRequirements") != deployment["externalRequirements"]):
        raise EvidenceError("Actual runtime closure inventory does not match complete program files/shared dependencies")
    bind_native_generation(closure, document, "Runtime closure")
    if not closure.get("rawEvidence"):
        raise MissingEvidence("Runtime closure lacks observed package/module/raw evidence")
    for row in closure["rawEvidence"]:
        reference(root, row)
    build_inputs = None
    if kind == "native-release":
        if deployment.get("selfExtractsRuntime") is not False:
            raise EvidenceError("A self-extracting dependency wrapper is not native portable single EXE")
        if document["platform"] == "windows" and (document["architecture"] != "x64" or deployment.get("windowsSingleExe") is not True or
                                                    len(runtime) != 1 or runtime[0]["role"] != "application"):
            raise EvidenceError("Native Windows x64 single EXE cannot require runtime sidecars")
        build = document.get("buildEvidence", {})
        for role in ("build-log", "link-command", "link-map", "dependency-inventory"):
            row = build.get(role)
            reference(root, row)
            if not any(item["sha256"] == row["sha256"] and item["role"] == role for item in ancillary):
                raise EvidenceError(f"Native Artifact manifest omits actual {role} material")
        build_inputs = verify_build_inputs(root, document)
    package = reference(root, document.get("programPackage"))
    method = document["programPackage"].get("compression")
    if not isinstance(method, dict) or not all(method.get(key) is not None for key in ("format", "algorithm", "level")):
        raise MissingEvidence("Program download package requires an explicit compression basis")
    if document["programPackage"].get("scope") != "program-runtime-only":
        raise EvidenceError("Game/Java/user-data or source bundles must not replace program package size")
    from archive_inventory import verify_program_package
    package_files = verify_program_package(package, method, runtime)
    generation = "current-palladium" if kind == "native-release" and product == "Palladium" else "historical-native-nickel" if kind == "native-release" else "legacy-nickel"
    mapping_summary = {key: value for key, value in source_mapping.items() if key != "sourceFiles"} if source_mapping else None
    return {"kind": kind, "product": document["product"], "canonicalProduct": product, "artifactGeneration": generation,
            "eligibleForCurrentNativeRelease": generation == "current-palladium", "snapshotMapping": mapping_summary,
            "executable": file_ref(executable, root), "runtimeFiles": runtime, "ancillaryFiles": ancillary,
            "runtimeDeployedBytes": sum(row["bytes"] for row in runtime),
            "ancillaryDeliveryBytes": sum(row["bytes"] for row in ancillary),
            "downloadBytes": package.stat().st_size, "compression": method, "packageFiles": package_files,
            "releaseProvenance": file_ref(provenance_path, root), "runtimeClosure": file_ref(closure_path, root),
            "externalRequirements": deployment["externalRequirements"], "buildInputs": build_inputs, "roles": dict(roles)}


def bind_report(root: Path, row: dict, executable_sha: str, *, kind: str | None = None) -> tuple[Path, dict]:
    path = reference(root, row)
    document = read_json(path)
    if document.get("executableSha256", document.get("sha256")) != executable_sha:
        raise EvidenceError("Report does not identify this exact final executable SHA-256")
    if kind and document.get("kind") != kind:
        raise EvidenceError(f"Wrong evidence scope, expected {kind}")
    return path, document


def acceptance_gate(root: Path, tool: dict, artifact: dict, modules: dict | None, output: Path) -> dict:
    parser = load_python_tool(root, tool, "nickel_existing_acceptance_audit")
    if not callable(getattr(parser, "audit", None)):
        raise EvidenceError("Existing acceptance audit_windows.py audit() entry point is required")
    executable = reference(root, artifact["executable"])
    module_path = reference(root, modules) if modules else None
    result = parser.audit(executable, module_path, None)
    write_json(output / "windows-acceptance-audit.json", result)
    if result.get("sha256") != artifact["executable"]["sha256"]:
        raise EvidenceError("Acceptance audited a different executable")
    if result.get("bitness") != 64 or result.get("machine") != "0x8664":
        raise EvidenceError("Windows release is not the required x64 executable")
    if result.get("importGate") != "passed" or result.get("failures"):
        return gate("failed", "Acceptance normal/delay import or observed module gate failed", audit=result,
                    parser=file_ref(reference(root, tool), root))
    if module_path is None or result.get("runtimeGate") != "passed-observed-scenarios":
        return gate("pending", "Normal/delay imports passed; actual loaded modules/children are still missing", audit=result,
                    parser=file_ref(reference(root, tool), root))
    observations = read_json(module_path)
    collection = observations.get("collection", {})
    if collection.get("origin") != "actual-windows-process-observation":
        raise EvidenceError("Runtime snapshots must have actual collector provenance, not manually filled module names")
    for snapshot in observations.get("snapshots", []):
        if not snapshot.get("capturedUtc") or snapshot.get("treeComplete") is not True:
            raise EvidenceError("Runtime snapshot lacks full observed process-tree/time evidence")
        for child in snapshot.get("children", []):
            if type(child.get("pid")) is not int or not child.get("creationTime100ns"):
                raise EvidenceError("Observed child lacks PID/creation identity")
        for process in snapshot.get("processTree", []):
            if process.get("moduleStatus") != "observed":
                raise EvidenceError("Runtime child module list was not actually observed")
            if process.get("purpose") == "launcher" and process["pid"] != snapshot["pid"]:
                raise EvidenceError("Portable native launcher has an unexplained application sidecar")
    return gate("passed", "Existing acceptance parser passed exact final EXE normal/delay and observed module/child audit",
                audit=result, parser=file_ref(reference(root, tool), root), modules=file_ref(module_path, root))


def function_gate(root: Path, row: dict, artifact: dict) -> dict:
    path, result = bind_report(root, row, artifact["executable"]["sha256"], kind="full-launcher-functional-acceptance")
    if result.get("scope") != "full-launcher" or result.get("sourceCommit") != artifact["sourceCommit"]:
        raise EvidenceError("Component, mock or stale source acceptance cannot satisfy complete delivery")
    bind_native_generation(result, artifact, "Functional/visual acceptance")
    tests = result.get("requirements", [])
    ids = {value.get("id") for value in tests}
    pending = sorted(FUNCTION_IDS - ids)
    failed = []
    for value in tests:
        if value.get("id") not in FUNCTION_IDS:
            continue
        if value.get("status") != "passed":
            (failed if value.get("status") == "failed" else pending).append(value["id"])
        elif not value.get("scenarios") or not value.get("rawEvidence"):
            pending.append(value["id"])
        else:
            for evidence in value["rawEvidence"]:
                reference(root, evidence)
    return gate("failed" if failed else "pending" if pending else "passed",
                "Actual complete launcher features, visual/state equivalence and security acceptance",
                failed=failed, pending=sorted(set(pending)), report=file_ref(path, root))


def legal_gate(root: Path, tool: dict, row: dict | None, artifact: dict, output: Path) -> dict:
    if not row:
        raise MissingEvidence("No final native corresponding source/objects/relink bundle")
    bundle = inside(root, row.get("path"), must_exist=True)
    manifest = reference(root, row.get("manifest"))
    if manifest.parent != bundle or manifest.name != "materials-manifest.json":
        raise EvidenceError("Legal bundle must identify its exact materials manifest")
    document = read_json(bundle / "input-materials.json")
    if document.get("scope") != "native-release":
        raise MissingEvidence("Existing asset-only/component legal package is not final native source/relink evidence")
    if document.get("releaseId") != artifact["releaseId"]:
        raise EvidenceError("Legal bundle release identity differs from the final Artifact")
    if document.get("build", {}).get("executable", {}).get("sha256") != artifact["executable"]["sha256"]:
        raise EvidenceError("Corresponding source/objects/relink bundle is not bound to final EXE")
    bind_native_generation(document, artifact, "Legal material input")
    checker = load_python_tool(root, tool, "nickel_existing_brand_legal")
    result = checker.verify_bundle(bundle)
    write_json(output / "brand-legal-audit.json", result)
    if not result.get("nativeLegalMaterialValidationPassed"):
        return gate("failed", "Existing brand legal verifier rejected native corresponding source/objects/relink", result=result)
    application = [value for value in document.get("components", []) if value.get("category") == "application"]
    source_archives = [value for item in application for value in item.get("correspondingSource", []) if value.get("kind") == "source-archive"]
    if not source_archives:
        raise EvidenceError("Final application corresponding source archive is missing")
    inputs = verify_build_inputs(root, artifact)
    expected = inputs["correspondingSourceArchive"]
    if not any(value.get("sha256") == expected["sha256"] and reference(bundle, value).stat().st_size == expected["bytes"]
               for value in source_archives):
        raise EvidenceError("Legal application source archive differs from the byte-verified actual build inputs")
    relink = document.get("relink", {})
    return gate("passed", "Existing native legal verifier and actual build-source/archive correspondence checked final materials",
                result=result, manifest=file_ref(manifest, root), checker=file_ref(reference(root, tool), root),
                buildInputs=inputs, applicationCode=relink.get("applicationCode", []), probe=relink.get("probe", {}))


def source_gate(root: Path, row: dict, artifact: dict) -> dict:
    path = reference(root, row)
    result = read_json(path)
    mapping = verify_snapshot_mapping(root, artifact)
    candidates = {artifact["sourceCommit"]}
    if mapping:
        candidates.add(mapping["originalSourceCommit"])
    if result.get("kind") != "native-final-source-audit" or result.get("candidate") not in candidates:
        raise EvidenceError("Final delivery source/ownership audit is not for the artifact commit or its verified original snapshot")
    if result.get("sourceSnapshotSha256") != artifact["sourceSnapshotSha256"] or result.get("baselineCount") != 442:
        raise EvidenceError("Final source SHA-256 snapshot or immutable 442-file baseline does not match")
    if result.get("status") not in {"passed", "pending", "failed"}:
        raise EvidenceError("Source audit lacks explicit gate status")
    if canonical_product(artifact["product"]) == "Palladium":
        files = result.get("sourceFiles")
        if not isinstance(files, list) or not files:
            raise MissingEvidence("Current Palladium source audit lacks the complete committed source-file records")
        if files != sorted(files, key=lambda value: value["path"]) or canonical_sha(files) != artifact["sourceSnapshotSha256"]:
            raise EvidenceError("Committed Palladium source records do not reproduce the declared snapshot digest")
        if mapping and (result.get("candidateTree") != mapping["sourceTree"] or files != mapping["sourceFiles"]):
            raise EvidenceError("Original source audit does not reproduce the actually verified CI tree/blob snapshot")
        identity = artifact["productIdentityInput"]
        source_path = relative_name(identity.get("sourcePath"))
        reference(root, identity)
        matches = [item for item in files if item.get("path") == source_path and
                   all(item.get(key) == identity[key] for key in ("sha256", "bytes"))]
        if len(matches) != 1:
            raise EvidenceError("Palladium consumed identity metadata is not byte-corresponding to its committed source snapshot")
    summary = {key: value for key, value in mapping.items() if key != "sourceFiles"} if mapping else None
    return gate(result["status"], "Frozen legacy bytes and whole multi-owner final source delta", report=file_ref(path, root),
                snapshotMapping=summary)


def clean_gate(root: Path, row: dict, artifact: dict) -> dict:
    path, result = bind_report(root, row, artifact["executable"]["sha256"], kind="windows-clean-deployment-evidence")
    if result.get("origin") != "actual-clean-windows-host-run" or not result.get("machine"):
        raise EvidenceError("A local dev process cannot be relabeled as clean-machine deployment")
    bind_native_generation(result, artifact, "Clean deployment")
    checks = result.get("checks", [])
    required = {"no-dev-path-dll", "no-runtime-extraction", "portable-move", "unicode-long-path",
                "read-only-location", "static-images-tls-dialogs", "offline-license-ui", "first-run-cli-portable-once"}
    observed = {value.get("id") for value in checks if value.get("status") == "passed" and value.get("rawEvidence")}
    for check in checks:
        for evidence in check.get("rawEvidence", []):
            reference(root, evidence)
    failed = [value["id"] for value in checks if value.get("status") == "failed"]
    absent = sorted(required - observed)
    return gate("failed" if failed else "pending" if absent else "passed", "Actual clean host, path/write and no-extraction scenarios",
                failed=failed, pending=absent, report=file_ref(path, root))


def platform_gate(root: Path, row: dict, expected: str, source_commit: str, *, artifact: dict | None = None) -> dict:
    path = reference(root, row)
    document = read_json(path)
    if document.get("kind") != "native-platform-release-evidence" or document.get("scope") != "full-launcher":
        raise EvidenceError("Core-only build/cross-compile is not full platform launcher runtime evidence")
    current = artifact is not None and canonical_product(artifact["product"]) == "Palladium"
    if (f"{document.get('platform')}-{document.get('architecture')}" != expected or
            not current and document.get("sourceCommit") != source_commit):
        raise EvidenceError("Platform/architecture/source identity mismatch")
    if not document.get("host") or document.get("executedOnTargetHost") is not True:
        raise MissingEvidence(f"No actual {expected} target host/runtime proof")
    artifact_path = reference(root, document.get("artifact", {}))
    if current:
        platform_artifact = read_json(artifact_path)
        verify_manifest(root, platform_artifact, expected_kind="native-release", require_current_native=True)
        if (artifact["sourceCommit"] != source_commit or not same_source_generation(root, artifact, platform_artifact) or
                platform_artifact["version"] != artifact["version"] or platform_artifact["product"] != artifact["product"] or
                f"{platform_artifact['platform']}-{platform_artifact['architecture']}" != expected):
            raise EvidenceError("Target-host Artifact does not identify the same Palladium source/product generation")
        bind_native_generation(document, platform_artifact, "Target-host release")
    for label in ("buildLog", "runtimeLog", "dependencyAudit"):
        reference(root, document.get(label, {}))
    if document.get("status") != "passed" or not document.get("scenarios"):
        return gate("failed" if document.get("status") == "failed" else "pending", "Platform runtime scenarios not complete")
    return gate("passed", "Actual target-host complete launcher package/build/runtime", report=file_ref(path, root),
                host=document["host"], scenarios=document["scenarios"])


def package_comparison(root: Path, row: dict, native: dict, measured: dict) -> dict:
    path = reference(root, row)
    legacy = read_json(path)
    old = verify_manifest(root, legacy)
    if (legacy["platform"], legacy["architecture"], old["compression"]) != (
            native["platform"], native["architecture"], measured["compression"]):
        raise EvidenceError("Package comparison platform/architecture/compression differs")
    common = legacy.get("equivalentFeatureEvidence")
    if not common or common != native.get("equivalentFeatureEvidence"):
        raise MissingEvidence("No shared hash-identified full feature equivalence evidence for size comparison")
    reference(root, common)
    old_size, new_size = old["downloadBytes"], measured["downloadBytes"]
    reduction = (old_size - new_size) / old_size
    return gate("passed" if reduction >= 0.20 else "failed", "Measured complete program package target; no forecast numbers",
                legacyDownloadBytes=old_size, nativeDownloadBytes=new_size, measuredReduction=reduction,
                targetReduction=0.20, legacyDeployedBytes=old["runtimeDeployedBytes"],
                nativeDeployedBytes=measured["runtimeDeployedBytes"], ancillaryDeliveryBytes=measured["ancillaryDeliveryBytes"],
                externalRequirements={"legacy": old["externalRequirements"], "native": measured["externalRequirements"]},
                compression=measured["compression"], legacyManifest=file_ref(path, root))


def performance_report_gate(root: Path, row: dict, artifact: dict) -> dict:
    path, report = bind_report(root, row, artifact["executable"]["sha256"], kind="full-launcher-performance-evidence")
    bind_native_generation(report, artifact, "Performance report")
    status = report.get("status")
    if status not in {"passed", "failed", "pending"}:
        raise EvidenceError("Performance report lacks verified gate semantics")
    if status == "passed":
        gates, runs = report.get("gates"), report.get("rawRuns")
        if not isinstance(gates, dict) or not gates or not isinstance(runs, list) or not runs:
            raise MissingEvidence("Passed performance needs real raw runs and explicit sample/lifetime/GPU gates")
        if overall(gates) != "passed":
            raise EvidenceError("Performance status contradicts its actual sample/observation gates")
        for role in ("legacy", "native"):
            for group in ("cold-app-cache", "hot-app-cache"):
                sample_gate = gates.get(f"samples:{role}:{group}", {})
                if (sample_gate.get("status") != "passed" or type(sample_gate.get("startupCount")) is not int or
                        sample_gate["startupCount"] < 10 or type(sample_gate.get("memoryCount")) is not int or sample_gate["memoryCount"] < 5):
                    raise MissingEvidence("Each actual launcher needs >=20 startups and >=10 memory samples split cold/hot")
        for run in runs:
            if not isinstance(run, dict) or not isinstance(run.get("context"), dict):
                raise EvidenceError("Performance raw run lacks its actual launch identity")
            context = run["context"]
            if context.get("role") == "native":
                expected = {"sourceCommit": artifact["sourceCommit"], "sourceSnapshotSha256": artifact["sourceSnapshotSha256"],
                            "buildSourceSha256": artifact["buildSourceSha256"], "executableSha256": artifact["executable"]["sha256"]}
                if any(context.get(key) != value for key, value in expected.items()):
                    raise EvidenceError("Raw native performance run identifies a different Palladium source/build/EXE generation")
            reference(root, run.get("samples"))
            reference(root, run.get("modules"))
            if run.get("startup"):
                reference(root, run["startup"].get("driverReceipt"))
    return gate(status, "Independent cold/hot tree CPU/GPU raw samples and equivalence gates",
                report=file_ref(path, root), gates=report.get("gates"))


def collect(root: Path, request: dict, output: Path) -> dict:
    gates, inputs, artifact, measured = {}, [], None, None

    def run(name, callback):
        try:
            gates[name] = callback()
        except (MissingEvidence, ModuleNotFoundError) as error:
            gates[name] = gate("pending", str(error))
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            gates[name] = gate("failed", str(error))

    def artifact_gate():
        nonlocal artifact, measured
        row = request.get("artifactManifest")
        if not row:
            raise MissingEvidence("No complete Portable EXE / Artifact manifest has been supplied")
        path = reference(root, row)
        candidate = read_json(path)
        if not isinstance(candidate, dict):
            raise EvidenceError("Artifact manifest must be a release evidence object")
        artifact = candidate
        measured = verify_manifest(root, artifact, expected_kind="native-release", require_current_native=True)
        if root == ROOT:
            inputs.append(copy_small_input(path, output, "artifact-manifest.raw.json"))
        return gate("passed", "Every Palladium runtime, actual build input and corresponding-source archive hashed from actual files",
                    manifest=file_ref(path, root), measured=measured)

    run("artifactManifest", artifact_gate)
    usable = artifact is not None and measured is not None
    if usable:
        run("sourceOwnership", lambda: source_gate(root, request.get("sourceAudit", {}), artifact))
        run("windowsImportsModulesChildren", lambda: acceptance_gate(root, request.get("acceptanceTool", {}), artifact,
                                                                   request.get("runtimeModules"), output))
        run("fullFunctionStateVisual", lambda: function_gate(root, request.get("functionalAcceptance", {}), artifact))
        run("finalSourceObjectsRelink", lambda: legal_gate(root, request.get("brandTool", {}), request.get("legalBundle", {}),
                                                         artifact, output))
        run("windowsCleanDeployment", lambda: clean_gate(root, request.get("cleanDeployment", {}), artifact))
        run("packageSize", lambda: package_comparison(root, request.get("legacyArtifactManifest", {}), artifact, measured))
        for platform in REQUIRED_PLATFORMS:
            run(platform, lambda value=platform: platform_gate(root, request.get("platforms", {}).get(value, {}), value,
                                                             artifact["sourceCommit"], artifact=artifact))
        if request.get("performanceReport"):
            run("performance", lambda: performance_report_gate(root, request["performanceReport"], artifact))
        else:
            gates["performance"] = gate("pending", "Missing equivalent full Release old/native 10 memory and 20 startup samples")
    else:
        for name in ("sourceOwnership", "windowsImportsModulesChildren", "fullFunctionStateVisual", "finalSourceObjectsRelink",
                     "windowsCleanDeployment", "packageSize", "performance", *REQUIRED_PLATFORMS):
            gates[name] = gate("pending", "No usable current Palladium Artifact; historical Nickel/component/probe/asset evidence does not substitute")
    status = overall(gates)
    result = {"schemaVersion": 1, "kind": "native-release-evidence", "createdUtc": utc_now(), "status": status,
              "nativeReleaseReady": status == "passed", "requestSha256": canonical_sha(request), "rawInputs": inputs,
              "product": artifact.get("product") if artifact else None,
              "artifactGeneration": measured.get("artifactGeneration") if measured else None,
              "executableSha256": artifact.get("executable", {}).get("sha256") if artifact else None,
              "sourceCommit": artifact.get("sourceCommit") if artifact else None,
              "sourceSnapshotSha256": artifact.get("sourceSnapshotSha256") if artifact else None,
              "buildSourceSha256": artifact.get("buildSourceSha256") if artifact else None,
              "gates": gates,
              "limitations": ["No gate infers a full product from foundation, assets, probes, components or test fixtures.",
                              "Historical Nickel artifacts remain unchanged and inspectable, never current Palladium acceptance.",
                              "PE parsing and legal material checks are delegated to their existing owned tools.",
                              "Only actual target-host runs satisfy Linux/macOS release gates; absent macOS host stays pending.",
                              "20%/50% are thresholds, never filled with guessed performance reductions."]}
    write_json(output / "release-evidence.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Hash references to actual builder/acceptance/brand evidence")
    parser.add_argument("--output", required=True, help="Unused directory under tests/native-release/_local/")
    args = parser.parse_args()
    output = new_output(args.output)
    try:
        request_path = inside(ROOT, args.input, must_exist=True)
        request = read_json(request_path)
        if request.get("schemaVersion") != 1:
            raise EvidenceError("Unsupported release collector request schema")
        copy_small_input(request_path, output, "collector-request.raw.json")
        result = collect(ROOT, request, output)
    except (OSError, EvidenceError) as error:
        result = {"schemaVersion": 1, "kind": "native-release-evidence", "status": "pending" if isinstance(error, MissingEvidence) else "failed",
                  "nativeReleaseReady": False, "gates": {"input": gate("pending" if isinstance(error, MissingEvidence) else "failed", str(error))}}
        write_json(output / "release-evidence.json", result)
    print(f"{result['status']}: {output.relative_to(ROOT).as_posix()}/release-evidence.json")
    return exit_code(result["status"])


if __name__ == "__main__":
    raise SystemExit(main())

"""Verify the supplied built-generation portal without rewriting historical manifests."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import stat
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evidence


def record_local(path: Path, root: Path, expected: str, size: int | None = None) -> dict:
    source = evidence.checked_local(path)
    digest = evidence.sha256(source)
    actual_size = source.stat().st_size
    if digest != evidence.require_hash(expected, "input pin") or (size is not None and actual_size != size):
        raise evidence.EvidenceError("frozen input differs from its bound bytes: " + str(source))
    return {"path": source.relative_to(root).as_posix(), "size": actual_size, "sha256": digest}


def hash_artifact(row: dict) -> dict:
    # External access is restricted to the exact artifact paths in the pinned portal.
    source = Path(row["physicalPath"])
    if not source.is_absolute() or source.suffix.lower() not in {".lib", ".exe"}:
        raise evidence.EvidenceError("invalid bound production artifact")
    before = source.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_size != row["bytes"]:
        raise evidence.EvidenceError("bound artifact type/size changed: " + str(source))
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        while chunk := stream.read(256 * 1024):
            digest.update(chunk)
    after = source.stat()
    identity = lambda info: (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
    if identity(before) != identity(after) or digest.hexdigest() != row["sha256"]:
        raise evidence.EvidenceError("bound artifact changed after the owner cutover: " + str(source))
    return {"logicalPath": str(source), "resolvedPhysicalPath": str(source.resolve(strict=True)),
            "size": after.st_size, "sha256": digest.hexdigest(), "handlesClosed": True}


def prepare(args) -> dict:
    root = evidence.checked_local(args.portal_root)
    manifest = root / "snapshot.json"
    portal_pin = evidence.require_hash(args.portal_sha256, "portal pin")
    source_pin = evidence.require_hash(args.source_sha256, "source-generation pin")
    if evidence.sha256(manifest) != portal_pin:
        raise evidence.EvidenceError("raw portal does not match the independently supplied pin")
    portal = evidence.read_json(manifest)
    if portal.get("kind") != "matched-built-production-artifact-source-portal" or portal.get("member") != "production-smoke":
        raise evidence.EvidenceError("not this receiver's built-generation portal")
    marker = root / "publication-complete.json"
    complete = evidence.read_json(marker)
    if (complete.get("publicationComplete") is not True or complete.get("receiver") != "production-smoke"
            or complete.get("receiverManifestSha256") != portal_pin
            or complete.get("sourceGenerationManifestSha256") != source_pin
            or portal.get("sourceGenerationManifestSha256") != source_pin):
        raise evidence.EvidenceError("completion does not bind the separate portal/source scopes")
    rows = []
    for relative, info in portal["files"].items():
        relative = evidence.relative_path(relative)
        rows.append(record_local(root / relative, root, info["sha256"], info["bytes"]))
    if len(rows) != complete["headerFiles"]:
        raise evidence.EvidenceError("actual header count differs from publication completion")
    for relative, info in portal["boundInputs"].items():
        relative = evidence.relative_path(relative)
        rows.append(record_local(root / "bound-inputs" / relative, root, info["sha256"], info["bytes"]))
    source_manifest = root / "bound-inputs/source-generation87-snapshot.json"
    rows.append(record_local(source_manifest, root, source_pin))
    upstream_path = root / "bound-inputs/upstream-artifact-manifest.json"
    rows.append(record_local(upstream_path, root, portal["upstreamArtifactManifestSha256"]))
    upstream = evidence.read_json(upstream_path)
    if upstream["sourceGenerationManifestSha256"] != source_pin:
        raise evidence.EvidenceError("upstream build material belongs to a different generation")
    commands = root / evidence.relative_path(portal["appCompileCommandsFile"])
    rows.append(record_local(commands, root, portal["appCompileCommandsSha256"]))
    for info in portal["generatedMoc"]:
        local = root / "bound-inputs/generated-moc" / Path(info["physicalPath"]).name
        rows.append(record_local(local, root, info["sha256"], info["bytes"]))
    if len({row["path"].casefold() for row in rows}) != len(rows):
        raise evidence.EvidenceError("duplicate frozen input names")
    minimum_path = evidence.checked_local(args.minimum_inputs)
    minimum_pin = evidence.require_hash(args.minimum_sha256, "minimum-input pin")
    if evidence.sha256(minimum_path) != minimum_pin:
        raise evidence.EvidenceError("received link input manifest changed")
    minimum = evidence.read_json(minimum_path)
    if minimum["portalManifestSha256"] != portal_pin or minimum["sourceGenerationManifestSha256"] != source_pin:
        raise evidence.EvidenceError("received link/common inputs belong to a different generation")
    for row in minimum["files"]:
        record_local(minimum_path.parent / evidence.relative_path(row["path"]), minimum_path.parent,
                     row["sha256"], row["size"])
    references = []
    if args.cutover_complete:
        evidence.capacity(args.output, 1024 * 1024, 256 * 1024 * 1024)
        if sys.platform == "win32":
            import shutil
            if shutil.disk_usage("C:/").free < 1024 * 1024 * 1024:
                raise evidence.EvidenceError("C is below the shared 1 GiB reserve")
        references = [hash_artifact(row) for row in portal["artifacts"]
                      if Path(row["physicalPath"]).suffix.lower() in {".lib", ".exe"}]
        if not references or not any(Path(row["logicalPath"]).suffix == ".exe" for row in references):
            raise evidence.EvidenceError("no complete production artifact closure")
    result = {"schema": 1, "kind": "verified-built-generation-inputs", "verifiedAt": evidence.utc_now(),
              "root": str(root), "portalManifest": str(manifest), "portalManifestSha256": portal_pin,
              "sourceManifest": str(source_manifest), "sourceGenerationManifestSha256": source_pin,
              "completeMarker": str(marker), "completeMarkerSha256": evidence.sha256(marker),
              "files": rows, "fileCount": len(rows), "totalBytes": sum(row["size"] for row in rows),
              "minimumInputs": str(minimum_path), "minimumInputsSha256": minimum_pin,
              "actualCTestInventory": str(evidence.checked_local(args.ctest_inventory)),
              "artifactReferences": references, "cutoverCompleteAssertedByCaller": args.cutover_complete,
              "artifactHandlesClosed": True, "copiedLibraryBytes": 0,
              "classification": "prepared-existing-matrix-inputs-runtime-and-CTest-still-required",
              "productionPass": False}
    if evidence.sha256(manifest) != portal_pin:
        raise evidence.EvidenceError("portal changed during verification")
    evidence.write_json_new(args.output, result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portal-root", required=True)
    parser.add_argument("--portal-sha256", required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--minimum-inputs", required=True)
    parser.add_argument("--minimum-sha256", required=True)
    parser.add_argument("--ctest-inventory", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cutover-complete", action="store_true",
                        help="Only after owner completion: rehash the exact portal artifact references")
    args = parser.parse_args()
    try:
        result = prepare(args)
        print("verified", result["fileCount"], "local inputs", result["totalBytes"], "bytes; artifacts",
              len(result["artifactReferences"]), "product PASS", result["productionPass"])
        return 0
    except (OSError, KeyError, TypeError, evidence.EvidenceError) as error:
        print("BLOCKED:", error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

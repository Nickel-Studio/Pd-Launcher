#!/usr/bin/env python3
"""Verify immutable input metadata; production behavior is tested only by C++."""
import argparse
import hashlib
import json
from pathlib import Path


OWNER = "posix-cleanup-acceptance"


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_root(shared, worktree, expected=None):
    for parent in (shared, *shared.parents):
        if parent == worktree:
            break
        if parent.is_symlink():
            raise RuntimeError(f"linked snapshot root: {parent}")
    shared = shared.resolve(strict=True)
    shared.relative_to(worktree / ".snow")
    manifest_path = shared / "snapshot.json"
    marker_path = shared / "publication-complete.json"
    if (shared / ".publisher.lock").exists():
        raise RuntimeError("publisher lock present")
    for path in (manifest_path, marker_path):
        if path.is_symlink():
            raise RuntimeError(f"symlink input: {path}")
    marker = json.loads(marker_path.read_text(encoding="utf-8-sig"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    receiver = manifest.get("member", manifest.get("metadata", {}).get("receiver"))
    if marker.get("publicationComplete") is not True or receiver != OWNER or manifest.get("schemaVersion") != 1:
        raise RuntimeError("publication incomplete or wrong receiver/schema")
    actual = digest(manifest_path)
    wanted = marker.get("receiverManifestSha256", marker.get("snapshotFileSha256"))
    if actual != wanted or (expected and actual != expected):
        raise RuntimeError("completion marker mismatch or immutable input changed")
    print(f"MANIFEST_SHA256 {actual} root={shared}")
    print(f"MARKER_SHA256 {digest(marker_path)} root={shared}")
    print(f"PUBLICATION finishedUtc={marker['finishedUtc']} rule={marker.get('readerLock', marker.get('readerRule'))}")
    totals = {}
    for section in ("files", "evidence"):
        total = 0
        for name, entry in sorted(manifest.get(section, {}).items()):
            relative = Path(name)
            if relative.is_absolute() or ".." in relative.parts or ":" in name or "\\" in name:
                raise RuntimeError(f"unsafe manifest path: {name}")
            path = shared / relative
            for parent in (path, *path.parents):
                if parent == worktree:
                    break
                if parent.is_symlink():
                    raise RuntimeError(f"linked input: {name}")
            path.resolve(strict=True).relative_to(shared)
            size = path.stat().st_size
            actual_file = digest(path)
            if actual_file != entry["sha256"] or size != entry["bytes"]:
                raise RuntimeError(f"frozen input mismatch: {name}")
            total += size
            print(f"INPUT_SHA256 {actual_file} {name} bytes={size} section={section} root={shared}")
        totals[section] = total
    if len(manifest["files"]) != marker["sourceFiles"] or totals["files"] != marker["sourceBytes"]:
        raise RuntimeError("publication file count/bytes mismatch")
    if len(manifest.get("evidence", {})) != marker.get("evidenceFiles", 0):
        raise RuntimeError("publication evidence count mismatch")
    print(f"FROZEN_INPUTS_PASS files={len(manifest['files'])} bytes={totals['files']} root={shared}")
    return manifest


def verify_zip(shared, manifest):
    import zipfile
    metadata = manifest.get("metadata", {})
    captured = metadata.get("sourceMode") == "immutable-git-transport-capture"
    if captured:
        manifest_name = metadata["upstreamManifest"]
        completion_name = metadata["upstreamCompletion"]
        zip_name = metadata["upstreamZip"]
        embedded_name = metadata["embeddedManifest"]
        if any(name not in manifest.get("evidence", {}) for name in (manifest_name, completion_name, zip_name)):
            raise RuntimeError("Git capture archive provenance missing")
    else:
        manifest_name = "upstream/posix-leaf-fix1-manifest.json"
        completion_name = "upstream/posix-leaf-fix1-complete.json"
        zip_name = "upstream/posix-leaf-fix1-sources.zip"
        embedded_name = "posix-leaf-fix1-manifest.json"
    upstream = json.loads((shared / completion_name).read_text())
    upstream_manifest = json.loads((shared / manifest_name).read_text())
    expected_owner = OWNER if captured else "transport"
    if upstream.get("publicationComplete") is not True or upstream.get("owner") != expected_owner or upstream.get("runtimePassed") is not False:
        raise RuntimeError("upstream capture/syntax-only completion missing")
    if captured:
        if upstream.get("syntaxPassed") is not False or upstream.get("stage") != "immutable-git-capture-not-runtime" or upstream.get("zipMembers") != 12:
            raise RuntimeError("Git capture must not claim syntax or runtime acceptance")
        if any(upstream.get(key) != metadata.get(key) or upstream_manifest.get(key) != metadata.get(key)
               for key in ("gitCommit", "gitTree")):
            raise RuntimeError("Git archive generation mismatch")
    zip_path = shared / zip_name
    if digest(zip_path) != upstream["zipSha256"] or zip_path.stat().st_size != upstream["bytes"] or digest(shared / manifest_name) != upstream["manifestSha256"]:
        raise RuntimeError("upstream provenance mismatch")
    entries = upstream_manifest["files"]
    upstream_files = {entry["path"]: {"sha256": entry["sha256"], "bytes": entry["bytes"]} for entry in entries}
    if len(entries) != len(upstream_files) or upstream_files != manifest["files"] or upstream["entries"] != 11 or len(upstream_files) != 11:
        raise RuntimeError("upstream selected 11-source closure mismatch")
    if not embedded_name or Path(embedded_name).name != embedded_name or embedded_name in upstream_files:
        raise RuntimeError("unsafe embedded manifest name")
    expected_members = dict(upstream_files)
    expected_members[embedded_name] = manifest["evidence"][manifest_name]
    with zipfile.ZipFile(zip_path) as archive:
        names = archive.namelist()
        if len(names) != 12 or len(names) != len(set(names)) or set(names) != set(expected_members):
            raise RuntimeError("ZIP is not exact 11 sources plus embedded manifest")
        for name in names:
            entry = expected_members[name]
            raw = archive.read(name)
            member_sha = hashlib.sha256(raw).hexdigest()
            if len(raw) != entry["bytes"] or member_sha != entry["sha256"]:
                raise RuntimeError(f"ZIP member mismatch: {name}")
            print(f"ZIP_MEMBER_SHA256 {member_sha} {name}")
    if captured:
        print(f"GIT_CAPTURE_ZIP_PASS sources=11 embedded_manifest=1 members=12 git_commit={metadata['gitCommit']} runtimePassed=false syntaxPassed=false")
    else:
        print("UPSTREAM_ZIP_PASS sources=11 embedded_manifest=1 runtimePassed=false")


def verify_composition(source, manifest, archive_root, archive_manifest):
    provenance = source / "provenance"
    if digest(provenance / "fix1-snapshot.json") != digest(archive_root / "snapshot.json"):
        raise RuntimeError("archive snapshot is not the composed root's fixed input")
    old = json.loads((provenance / "old-snapshot.json").read_text(encoding="utf-8-sig"))
    old_marker = json.loads((provenance / "old-publication-complete.json").read_text(encoding="utf-8-sig"))
    if old_marker.get("publicationComplete") is not True or old_marker["receiverManifestSha256"] != digest(provenance / "old-snapshot.json"):
        raise RuntimeError("original provenance completion mismatch")
    if digest(provenance / "old-snapshot.json") != manifest["metadata"]["originalSnapshotSha256"] or digest(archive_root / "snapshot.json") != manifest["metadata"]["fix1SnapshotSha256"]:
        raise RuntimeError("composed provenance identity mismatch")
    expected = {name: {"sha256": entry["sha256"], "bytes": entry["bytes"]} for name, entry in old["files"].items()}
    if len(expected) != 24 or len(archive_manifest["files"]) != 11 or not set(archive_manifest["files"]).issubset(expected):
        raise RuntimeError("composition is not 13 preserved plus 11 fixed inputs")
    expected.update(archive_manifest["files"])
    if expected != manifest["files"] or manifest["metadata"]["preservedEntries"] != 13 or manifest["metadata"]["repairedEntries"] != 11:
        raise RuntimeError("composed production root mismatch")
    print(f"COMPOSED_ROOT_PROVENANCE_PASS files=24 preserved=13 fixed=11 production_only={source}")


def verify_git_capture(source, manifest, archive_root, archive_manifest):
    metadata = manifest["metadata"]
    for key in ("gitCommit", "gitTree"):
        value = metadata.get(key, "")
        if len(value) != 40 or any(char not in "0123456789abcdef" for char in value):
            raise RuntimeError("invalid immutable Git identity")
    provenance_name = metadata.get("gitObjectManifest")
    if provenance_name not in manifest.get("evidence", {}):
        raise RuntimeError("Git object provenance not in the completed root")
    provenance = json.loads((source / provenance_name).read_text())
    if any(provenance.get(key) != metadata[key] for key in ("gitCommit", "gitTree")):
        raise RuntimeError("source Git object generation mismatch")
    objects = provenance["files"]
    objects_by_path = {entry["path"]: entry for entry in objects}
    if len(objects) != 24 or len(objects_by_path) != 24 or set(objects_by_path) != set(manifest["files"]):
        raise RuntimeError("Git object closure is not the exact 24-file root")
    for name, entry in objects_by_path.items():
        expected = manifest["files"][name]
        raw = (source / name).read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw).hexdigest()
        if entry["sha256"] != expected["sha256"] or entry["bytes"] != expected["bytes"] or blob != entry["gitBlob"]:
            raise RuntimeError(f"captured Git object mismatch: {name}")
    archive_metadata = archive_manifest.get("metadata", {})
    if archive_metadata.get("sourceMode") != "immutable-git-transport-capture" or any(
            archive_metadata.get(key) != metadata[key] for key in ("gitCommit", "gitTree")):
        raise RuntimeError("transport archive does not belong to the selected immutable commit")
    if digest(archive_root / "snapshot.json") != metadata["transportSnapshotSha256"] or digest(source / "provenance/transport-snapshot.json") != digest(archive_root / "snapshot.json"):
        raise RuntimeError("transport snapshot identity mismatch")
    if digest(source / "provenance/transport-publication-complete.json") != digest(archive_root / "publication-complete.json"):
        raise RuntimeError("transport completion identity mismatch")
    if len(archive_manifest["files"]) != 11 or not set(archive_manifest["files"]).issubset(manifest["files"]):
        raise RuntimeError("transport closure is not the expected 11-file subset")
    for name, entry in archive_manifest["files"].items():
        if entry != manifest["files"][name]:
            raise RuntimeError("transport subset differs from the production root")
    required = ("native/core/config/conditional_file.hpp", "native/core/config/conditional_file_impl.inc", "native/core/config/windows_security.hpp")
    if any(name not in manifest["files"] for name in required):
        raise RuntimeError("current atomic-file direct dependencies missing")
    print(f"IMMUTABLE_GIT_ROOT_PASS files=24 transport_sources=11 git_commit={metadata['gitCommit']} production_only={source}")


def verify_manifest_zip(source, manifest_name, zip_name, entries_key, count):
    import zipfile
    document = json.loads((source / manifest_name).read_text(encoding="utf-8-sig"))
    entries = document[entries_key]
    files = {entry["path"]: {"sha256": entry["sha256"], "bytes": entry["bytes"]} for entry in entries}
    if len(entries) != count or len(files) != count:
        raise RuntimeError("upstream archive source count or duplicate path mismatch")
    for name in files:
        if Path(name).is_absolute() or ".." in Path(name).parts or ":" in name or "\\" in name:
            raise RuntimeError("unsafe upstream archive path")
    embedded = Path(manifest_name).name
    expected = dict(files)
    expected[embedded] = {"sha256": digest(source / manifest_name), "bytes": (source / manifest_name).stat().st_size}
    with zipfile.ZipFile(source / zip_name) as archive:
        names = archive.namelist()
        if len(names) != count + 1 or len(names) != len(set(names)) or set(names) != set(expected):
            raise RuntimeError("upstream ZIP is not its exact sources plus embedded manifest")
        for name, entry in expected.items():
            if archive.getinfo(name).file_size != entry["bytes"]:
                raise RuntimeError("upstream ZIP declared length mismatch")
            raw = archive.read(name)
            if len(raw) != entry["bytes"] or hashlib.sha256(raw).hexdigest() != entry["sha256"]:
                raise RuntimeError(f"upstream ZIP member mismatch: {name}")
    print(f"EXACT_UPSTREAM_ZIP_PASS source_entries={count} embedded_manifest=1 members={count + 1} zip={zip_name} sha256={digest(source / zip_name)}")
    return document, files


def verify_canonical_source1(source, manifest):
    metadata = manifest["metadata"]
    if metadata.get("singleProductionRoot") is not True or metadata.get("productionSourceFiles") != 24 or metadata.get("currentTransportReplacementFiles") != 11:
        raise RuntimeError("canonical source1 is not the required complete single root")
    if metadata.get("runtimePassed") is not False or metadata.get("previousFix1RuntimeNotInherited") is not True or metadata.get("securityQualification") != "NOT_SECURITY_PASS":
        raise RuntimeError("canonical source publication cannot inherit a runtime/security PASS")
    prefix = "upstream/"
    posix_name = prefix + "canonical-posix-source1-manifest.json"
    zip_name = prefix + "canonical-posix-source1-sources.zip"
    if digest(source / posix_name) != metadata["embeddedSourceManifestSha256"] or digest(source / zip_name) != metadata["embeddedTwelveMemberZipSha256"]:
        raise RuntimeError("canonical embedded 12-member archive binding mismatch")
    posix, selected = verify_manifest_zip(source, posix_name, zip_name, "files", 11)
    old_name = prefix + "old-fix1-root-snapshot.json"
    old_marker_name = prefix + "old-fix1-root-publication-complete.json"
    old = json.loads((source / old_name).read_text())
    old_marker = json.loads((source / old_marker_name).read_text())
    if old_marker.get("publicationComplete") is not True or old_marker["snapshotFileSha256"] != digest(source / old_name):
        raise RuntimeError("canonical source baseline completion mismatch")
    old_transport = json.loads((source / (prefix + "old-eleven-source-snapshot.json")).read_text())
    if set(selected) != set(old_transport["files"]) or not set(selected).issubset(old["files"]):
        raise RuntimeError("canonical selected transport paths changed")
    expected = dict(old["files"])
    expected.update(selected)
    if len(expected) != 24 or expected != manifest["files"]:
        raise RuntimeError("canonical root differs from 13 retained plus 11 current files")
    if selected["native/core/tasks/secure_fs.cpp"]["sha256"] != metadata["secureFsSha256"]:
        raise RuntimeError("canonical secure_fs SHA differs from selected inputs")
    for stem, key, count in (("canonical-mutation-source1", "selectedFiles", 44),
                             ("canonical-mutation-source1-audit", "entries", 37)):
        full_manifest_name = prefix + stem + "-manifest.json"
        complete = json.loads((source / (prefix + stem + "-complete.json")).read_text())
        full_zip_name = prefix + stem + ("-sources.zip" if count == 44 else "-inputs.zip")
        if complete.get("publicationComplete") is not True or complete.get("owner") != "transport" or digest(source / full_manifest_name) != complete["manifest"]["sha256"] or digest(source / full_zip_name) != complete["zip"]["sha256"] or (source / full_zip_name).stat().st_size != complete["zip"]["bytes"]:
            raise RuntimeError("canonical upstream publication hash/length mismatch")
        document, full_files = verify_manifest_zip(source, full_manifest_name, full_zip_name, key, count)
        if count == 44:
            if complete.get("selectedEntries") != 44 or complete.get("posixNewGenerationPassed") is not False or complete.get("runtimePassed") is not False:
                raise RuntimeError("transport full/source checkpoint cannot count as fresh POSIX runtime")
            if digest(source / full_manifest_name) != metadata["upstreamSourceManifestSha256"] or posix["upstreamSourceManifestSha256"] != metadata["upstreamSourceManifestSha256"]:
                raise RuntimeError("canonical source manifest identity mismatch")
            for name, entry in selected.items():
                if not name.startswith("native/core/common/") and full_files.get(name) != entry:
                    raise RuntimeError("canonical transport ZIP differs from selected production bytes")
        else:
            if complete.get("entries") != 37 or posix["upstreamAuditManifestSha256"] != digest(source / full_manifest_name):
                raise RuntimeError("canonical audit manifest identity mismatch")
            for name, entry in selected.items():
                if name.startswith("native/core/common/") and full_files.get(name) != entry:
                    raise RuntimeError("canonical common inputs differ from bounded audit ZIP")
    origins = metadata["sourceOrigins"]
    if set(origins) != set(manifest["files"]):
        raise RuntimeError("canonical origin path set mismatch")
    for name, entry in origins.items():
        if any(entry[key] != manifest["files"][name][key] for key in ("sha256", "bytes")):
            raise RuntimeError("canonical per-source provenance mismatch")
    print(f"CANONICAL_SOURCE1_ROOT_PASS files=24 preserved=13 current=11 embedded_zip_members=12 upstream_sources=44 audit_inputs=37 secure_fs_sha256={metadata['secureFsSha256']} runtimePassed=false NOT_SECURITY_PASS")


def copy_provenance(source, manifest, archive_root, archive_manifest, destination, worktree):
    destination = destination.resolve(strict=True)
    destination.relative_to(worktree / ".snow/posix-cleanup-acceptance/evidence")
    copies = [(source / name, name) for name in manifest.get("evidence", {})]
    if archive_root is not None:
        copies.extend((archive_root / name, target) for name, target in (
            ("snapshot.json", "archive-snapshot.json"),
            ("publication-complete.json", "archive-publication-complete.json")))
        copies.extend((archive_root / name, name) for name in archive_manifest.get("evidence", {}))
    if len({name for _, name in copies}) != len(copies):
        raise RuntimeError("provenance evidence destination collision")
    for path, name in copies:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        for parent in (target, *target.parents):
            if parent == destination:
                break
            if parent.is_symlink():
                raise RuntimeError("linked evidence destination")
        raw = path.read_bytes()
        with target.open("xb") as stream:
            stream.write(raw)
        print(f"COPIED_PROVENANCE_SHA256 {hashlib.sha256(raw).hexdigest()} {name} bytes={len(raw)}")


def verify_build(build, source, manifest, worktree):
    import shlex
    from collections import Counter
    build = build.resolve(strict=True)
    build.relative_to(worktree / ".snow/posix-cleanup-acceptance/build")
    entries = json.loads((build / "compile_commands.json").read_text())
    expected = Counter({str(source / name): 1 for name in (
        "native/core/diagnostics/junk.cpp", "native/core/config/atomic_file.cpp", "native/core/config/path_guard.cpp",
        "native/core/config/detail.cpp", "native/core/config/crypto.cpp", "native/core/network/control.cpp", "native/core/network/hash.cpp")})
    expected[str(source / "native/core/tasks/secure_fs.cpp")] = 2
    test_file = worktree / "tests/native-posix-cleanup/acceptance.cpp"
    expected[str(test_file)] = 2
    actual = Counter()
    for entry in entries:
        path = Path(entry["file"]).resolve(strict=True)
        actual[str(path)] += 1
        command = entry.get("arguments") or shlex.split(entry["command"])
        if f"-I{source / 'native'}" not in command or str(worktree / ".snow/native-shared/native") in " ".join(command):
            raise RuntimeError("compile command does not bind solely to the selected production root")
        if path != test_file:
            relative = path.relative_to(source).as_posix()
            if digest(path) != manifest["files"][relative]["sha256"]:
                raise RuntimeError("compiled production file SHA mismatch")
            print(f"COMPILED_SOURCE_SHA256 {digest(path)} {relative}")
    if actual != expected:
        raise RuntimeError("compiled translation units do not match the explicit production closure")
    print(f"BUILD_SOURCES_PASS translation_units=11 production_units=9 test_units=2 production_only={source}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worktree", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--archive-root", type=Path)
    parser.add_argument("--expect-manifest")
    parser.add_argument("--expect-base-manifest")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--copy-provenance-to", type=Path)
    args = parser.parse_args()
    root = args.worktree.resolve(strict=True)
    if args.expect_base_manifest:
        verify_root(root / ".snow/native-shared", root, args.expect_base_manifest)
    source = args.source_root.resolve(strict=True)
    source_manifest = verify_root(args.source_root, root, args.expect_manifest)
    archive_root = None
    archive_manifest = None
    metadata = source_manifest.get("metadata", {})
    if "embeddedTwelveMemberZipSha256" in metadata:
        if args.archive_root is not None:
            raise RuntimeError("canonical source1 carries its complete upstream evidence in the single root")
        verify_canonical_source1(source, source_manifest)
    elif metadata.get("sourceMode") == "immutable-git-commit":
        if args.archive_root is None:
            raise RuntimeError("explicit 12-member transport archive required for current Git production inputs")
        archive_root = args.archive_root.resolve(strict=True)
        archive_manifest = verify_root(args.archive_root, root, metadata["transportSnapshotSha256"])
        verify_zip(archive_root, archive_manifest)
        verify_git_capture(source, source_manifest, archive_root, archive_manifest)
    elif "provenance/fix1-snapshot.json" in source_manifest.get("evidence", {}):
        if args.archive_root is None:
            raise RuntimeError("explicit archive evidence root required for composed fix1 inputs")
        archive_root = args.archive_root.resolve(strict=True)
        archive_manifest = verify_root(args.archive_root, root, source_manifest["metadata"]["fix1SnapshotSha256"])
        verify_zip(archive_root, archive_manifest)
        verify_composition(source, source_manifest, archive_root, archive_manifest)
    elif "upstream/posix-leaf-fix1-sources.zip" in source_manifest.get("evidence", {}):
        verify_zip(source, source_manifest)
    required = ("native/core/diagnostics/junk.hpp", "native/core/diagnostics/junk.cpp", "native/core/config/atomic_file.cpp",
                "native/core/config/path_guard.cpp", "native/core/config/detail.cpp", "native/core/config/crypto.cpp",
                "native/core/tasks/secure_fs.hpp", "native/core/tasks/secure_fs.cpp", "native/core/tasks/test_hooks.hpp",
                "native/core/network/hash.cpp", "native/core/network/control.cpp")
    if len(source_manifest["files"]) != 24 or any(name not in source_manifest["files"] for name in required):
        raise RuntimeError("selected source is not the complete 24-file production root; no fallback allowed")
    if args.build_dir:
        verify_build(args.build_dir, source, source_manifest, root)
    if args.copy_provenance_to:
        copy_provenance(source, source_manifest, archive_root, archive_manifest, args.copy_provenance_to, root)
    print(f"BUILD_INPUT_BINDING production_only={source} archive_evidence_only={args.archive_root}")
    for path in sorted((root / "tests/native-posix-cleanup").rglob("*")):
        if path.is_file() and "__pycache__" not in path.parts:
            print(f"TEST_SHA256 {digest(path)} {path.relative_to(root).as_posix()} bytes={path.stat().st_size}")


if __name__ == "__main__":
    main()

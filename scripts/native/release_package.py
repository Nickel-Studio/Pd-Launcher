#!/usr/bin/env python3
"""Build and independently verify a Palladium Windows release package.

Scope: a real *program* package (the installed program file set), plus the
corresponding-source archive of one exact Git commit and a dependency/SBOM
input inventory derived only from tracked pins and the actual link command.

This tool deliberately makes no release claim: it does not sign, does not run
the program, does not judge licenses and does not prove clean-machine
behaviour.

Sub-commands
------------
build   Create ``pd-launcher-windows-x64-<version>.zip`` (program files only,
        one clean top-level directory), ``SHA256SUMS``, a ``git archive``
        source ZIP of the exact commit, ``sbom-inputs.json`` and
        ``package-manifest.json`` inside the output directory.
verify  Re-open the ZIP, compare every member byte/SHA to the installed
        files, reject extra/foreign members, check ``SHA256SUMS``, prove the
        source archive equals the exact Git commit (member set + every blob
        hash), prove no untracked member exists, and *disclose* any tracked
        file that looks like build output instead of silently rewriting the
        commit.

Everything is standard library only.  Paths and hashes are reported, never
invented; anything not derivable from tracked inputs is ``null``.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile

SCHEMA_VERSION = 1

# Extensions that must never appear in the *program* package on Windows.
FORBIDDEN_PROGRAM_EXTENSIONS = {
    ".dll", ".pdb", ".lib", ".obj", ".exp", ".ilk", ".map", ".qml", ".js",
    ".mjs", ".cjs", ".pyd", ".node", ".rlib", ".rs", ".wasm", ".pak", ".dat",
}

# A source archive from `git archive` must never contain compiled output.
FORBIDDEN_SOURCE_DIRECTORIES = {
    "build", "dist", "target", "node_modules", ".git", "artifacts",
}
FORBIDDEN_SOURCE_EXTENSIONS = {
    ".obj", ".lib", ".pdb", ".exe", ".dll", ".ilk", ".exp", ".pyc",
    ".zip", ".7z", ".tar", ".gz", ".xz",
}

FIXED_ZIP_DATETIME = (1980, 1, 1, 0, 0, 0)


class PackageError(RuntimeError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_git(repo: Path, *args: str, binary: bool = False):
    completed = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise PackageError(
            "git %s failed (%d): %s"
            % (" ".join(args), completed.returncode,
               completed.stderr.decode("utf-8", "replace").strip())
        )
    return completed.stdout if binary else completed.stdout.decode("utf-8")


def posix_name(name: str) -> str:
    name = name.replace("\\", "/")
    pure = PurePosixPath(name)
    if pure.is_absolute() or ".." in pure.parts:
        raise PackageError("Unsafe archive member name: %r" % name)
    return pure.as_posix()


# ---------------------------------------------------------------------------
# Installed program set
# ---------------------------------------------------------------------------

def installed_program_files(installed_dir: Path, expected_exe: str) -> list[dict]:
    installed_dir = installed_dir.resolve()
    if not installed_dir.is_dir():
        raise PackageError("Installed directory does not exist: %s" % installed_dir)
    entries = sorted(installed_dir.rglob("*"))
    if not entries:
        raise PackageError("Installed directory is empty")
    rows = []
    for entry in entries:
        relative = entry.relative_to(installed_dir).as_posix()
        if entry.is_dir():
            raise PackageError(
                "Installed layout has an unexpected directory: %s" % relative)
        extension = entry.suffix.lower()
        if extension in FORBIDDEN_PROGRAM_EXTENSIONS:
            raise PackageError(
                "Installed layout has a forbidden sidecar/artifact: %s" % relative)
        if entry.name != expected_exe:
            raise PackageError(
                "Installed layout has an unexpected program file: %s" % relative)
        rows.append({
            "distributionPath": relative,
            "bytes": entry.stat().st_size,
            "sha256": sha256_file(entry),
        })
    if not any(row["distributionPath"] == expected_exe for row in rows):
        raise PackageError("Expected program file %r is missing" % expected_exe)
    return rows


# ---------------------------------------------------------------------------
# Program ZIP (deterministic)
# ---------------------------------------------------------------------------

def build_program_zip(installed_dir: Path, rows: list[dict], zip_path: Path,
                      top_level: str) -> dict:
    if zip_path.exists():
        raise PackageError("Program ZIP already exists: %s" % zip_path)
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=9) as archive:
        directory = zipfile.ZipInfo(top_level + "/", date_time=FIXED_ZIP_DATETIME)
        directory.create_system = 0
        directory.external_attr = 0x10  # MS-DOS directory attribute
        archive.writestr(directory, b"")
        for row in rows:
            info = zipfile.ZipInfo("%s/%s" % (top_level, row["distributionPath"]),
                                   date_time=FIXED_ZIP_DATETIME)
            info.create_system = 0
            info.external_attr = 0x20  # FILE_ATTRIBUTE_ARCHIVE
            info.compress_type = zipfile.ZIP_DEFLATED
            # The archive-level `compresslevel=9` above only seeds members whose
            # ZipInfo zipfile creates itself (writestr(str)/write()); an
            # explicitly constructed ZipInfo takes its level from the member
            # itself, so the level must be passed per member through the public
            # writestr(compresslevel=) keyword (documented since Python 3.7).
            # Without it, members deflate at zlib's default level 6 while the
            # manifest below still claims "deflate-level-9" (measured on the
            # 0.1.0 EXE: 12,345,794 B instead of 11,876,600 B, +3.80%).  If a
            # future Python removes the keyword, writestr() raises TypeError
            # and packaging fails loudly instead of silently regressing; the
            # per-member level is additionally asserted to engage by
            # tests/native-release/test_release_package_zip.py.
            archive.writestr(info, (installed_dir / row["distributionPath"]).read_bytes(),
                             compresslevel=9)
    return {
        "path": zip_path.name,
        "bytes": zip_path.stat().st_size,
        "sha256": sha256_file(zip_path),
        "format": "zip",
        "compression": "deflate-level-9",
        "topLevelDirectory": top_level,
        "members": [dict(row, archivePath="%s/%s" % (top_level, row["distributionPath"]))
                    for row in rows],
    }


def verify_program_zip(zip_path: Path, installed_dir: Path,
                       top_level: str) -> list[dict]:
    installed_dir = installed_dir.resolve()
    actual = {}
    for entry in sorted(installed_dir.rglob("*")):
        if entry.is_dir():
            continue
        actual[entry.relative_to(installed_dir).as_posix()] = entry
    seen = []
    with zipfile.ZipFile(zip_path) as archive:
        if archive.testzip() is not None:
            raise PackageError("Program ZIP failed its CRC test")
        seen_basenames = set()
        for item in archive.infolist():
            name = posix_name(item.filename)
            if item.is_dir():
                if name.rstrip("/") != top_level:
                    raise PackageError("Unexpected ZIP directory: %s" % name)
                continue
            parts = PurePosixPath(name).parts
            if len(parts) != 2 or parts[0] != top_level:
                raise PackageError("Member is not directly inside %r: %s"
                                   % (top_level, name))
            member = parts[1]
            if member in seen_basenames:
                raise PackageError("Duplicate ZIP member: %s" % member)
            seen_basenames.add(member)
            extension = PurePosixPath(member).suffix.lower()
            if extension in FORBIDDEN_PROGRAM_EXTENSIONS:
                raise PackageError("Forbidden member in program ZIP: %s" % member)
            if member not in actual:
                raise PackageError(
                    "ZIP contains a file that is not in the installed set: %s" % member)
            content = archive.read(item)
            digest = sha256_bytes(content)
            installed_bytes = actual[member].read_bytes()
            if content != installed_bytes:
                raise PackageError("ZIP member bytes differ from installed file: %s"
                                   % member)
            if item.file_size != len(content):
                raise PackageError("ZIP member size drift: %s" % member)
            seen.append({
                "distributionPath": member,
                "archivePath": name,
                "bytes": len(content),
                "sha256": digest,
                "installedSha256": sha256_file(actual[member]),
            })
    missing = sorted(set(actual) - seen_basenames)
    if missing:
        raise PackageError("Installed files missing from ZIP: %s" % ", ".join(missing))
    for row in seen:
        if row["sha256"] != row["installedSha256"]:
            raise PackageError("Member SHA mismatch for %s" % row["distributionPath"])
    return sorted(seen, key=lambda row: row["distributionPath"])


# ---------------------------------------------------------------------------
# Corresponding source archive
# ---------------------------------------------------------------------------

def tracked_tree(repo: Path, commit: str) -> dict[str, tuple[str, str]]:
    raw = run_git(repo, "ls-tree", "-r", "-z", commit)
    tree: dict[str, tuple[str, str]] = {}
    for record in raw.split("\0"):
        if not record:
            continue
        header, _, path = record.partition("\t")
        mode, kind, oid = header.split()
        if kind != "blob":
            raise PackageError("Unsupported tree entry %r (%s)" % (path, kind))
        tree[path] = (mode, oid)
    return tree


def create_source_archive(repo: Path, commit: str, output: Path,
                          prefix: str) -> dict:
    if output.exists():
        raise PackageError("Source archive already exists: %s" % output)
    resolved = run_git(repo, "rev-parse", commit).strip()
    if resolved != commit and not resolved.startswith(commit):
        raise PackageError("Commit identity mismatch: %s vs %s" % (commit, resolved))
    data = run_git(repo, "archive", "--format=zip", "--prefix=%s/" % prefix,
                   resolved, binary=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    tree = tracked_tree(repo, resolved)
    return {
        "path": output.name,
        "bytes": output.stat().st_size,
        "sha256": sha256_file(output),
        "format": "zip (git archive)",
        "prefix": prefix + "/",
        "commit": resolved,
        "trackedFiles": len(tree),
    }


def blob_content_map(repo: Path, oids: list[str]) -> dict[str, bytes]:
    """Actual blob contents for the given object ids (commit truth)."""
    unique = list(dict.fromkeys(oids))
    request = "".join(oid + "\n" for oid in unique).encode("ascii")
    completed = subprocess.run(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        input=request, capture_output=True, check=False, timeout=900,
    )
    if completed.returncode:
        raise PackageError("git cat-file --batch failed: %s"
                           % completed.stderr.decode("utf-8", "replace").strip())
    output = completed.stdout
    contents: dict[str, bytes] = {}
    position = 0
    for oid in unique:
        newline = output.find(b"\n", position)
        if newline < 0:
            raise PackageError("Truncated git cat-file output")
        header = output[position:newline].decode("ascii", "replace").strip()
        position = newline + 1
        parts = header.split()
        if len(parts) != 3 or parts[1] != "blob":
            raise PackageError("Unexpected git cat-file header: %r" % header)
        size = int(parts[2])
        content = output[position:position + size]
        if len(content) != size:
            raise PackageError("Short blob read for %s" % oid)
        position += size + 1  # trailing newline
        contents[oid] = content
    if len(contents) != len(unique):
        raise PackageError("Blob content map is incomplete")
    return contents


def verify_source_archive(repo: Path, commit: str, archive_path: Path,
                          prefix: str) -> dict:
    resolved = run_git(repo, "rev-parse", commit).strip()
    tree = tracked_tree(repo, resolved)
    members: dict[str, dict] = {}
    suspicious: list[dict] = []
    with zipfile.ZipFile(archive_path) as archive:
        if archive.testzip() is not None:
            raise PackageError("Source archive failed its CRC test")
        for item in archive.infolist():
            name = posix_name(item.filename)
            if item.is_dir():
                if not (name == prefix.rstrip("/") or name.startswith(prefix)):
                    raise PackageError("Unexpected source ZIP directory: %s" % name)
                continue
            if not name.startswith(prefix):
                raise PackageError("Source member outside %r: %s" % (prefix, name))
            relative = name[len(prefix):]
            if relative in members:
                raise PackageError("Duplicate source member: %s" % relative)
            if relative not in tree:
                raise PackageError(
                    "Source archive contains an untracked member: %s" % relative)
            extension = PurePosixPath(relative).suffix.lower()
            in_build_directory = any(
                part in FORBIDDEN_SOURCE_DIRECTORIES or part.startswith(".build")
                for part in PurePosixPath(relative).parts[:-1])
            if extension in FORBIDDEN_SOURCE_EXTENSIONS or in_build_directory:
                suspicious.append({"path": relative,
                                   "bytes": item.file_size,
                                   "trackedInCommit": True})
            members[relative] = {
                "bytes": item.file_size,
                "sha256": sha256_bytes(archive.read(item)),
            }
    missing = sorted(set(tree) - set(members))
    extra = sorted(set(members) - set(tree))
    if missing or extra:
        raise PackageError(
            "Source archive does not match the commit tree (missing=%d, extra=%d): %s %s"
            % (len(missing), len(extra), missing[:5], extra[:5]))
    contents = blob_content_map(repo, [oid for _mode, oid in tree.values()])
    mismatched = []
    for path, (_mode, oid) in tree.items():
        blob = contents[oid]
        if members[path]["sha256"] != sha256_bytes(blob):
            mismatched.append(path)
    crlf_exported: list[str] = []
    if mismatched:
        with zipfile.ZipFile(archive_path) as archive:
            for path in mismatched:
                member_bytes = archive.read(prefix + path)
                blob = contents[tree[path][1]]
                crlf = blob.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
                if member_bytes != crlf:
                    raise PackageError(
                        "Source member content is neither the commit blob nor its "
                        "declared CRLF export: %s" % path)
                crlf_exported.append(path)
    return {
        "path": archive_path.name,
        "bytes": archive_path.stat().st_size,
        "sha256": sha256_file(archive_path),
        "commit": resolved,
        "prefix": prefix + "/",
        "memberCount": len(members),
        "verifiedMemberSetEqualsCommitTree": True,
        "verifiedMemberContentEqualsCommitBlobs": True,
        "crlfExportedMemberCount": len(crlf_exported),
        "crlfExportedMembers": crlf_exported,
        "untrackedMembers": [],
        "suspiciousTrackedArtifacts": suspicious,
        "trackedSnowMembers": sorted(path for path in members
                                     if path.startswith(".snow/")),
    }


# ---------------------------------------------------------------------------
# Link inputs and dependency/SBOM inventory
# ---------------------------------------------------------------------------

def link_inputs(build_ninja: Path, target: str) -> list[str]:
    text = build_ninja.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("build ") and target in line and ":" in line:
            block = [line]
            for follow in lines[index + 1:index + 40]:
                if follow.startswith("build "):
                    break
                block.append(follow)
            joined = "\n".join(block)
            match = re.search(r"^  LINK_LIBRARIES = (.*)$", joined, re.MULTILINE)
            if match:
                return [token for token in match.group(1).split() if token]
    raise PackageError("Final link command for %r was not found" % target)


PINNED_BY_LIBRARY = {
    "zlibstatic.lib": "zlib",
    "archive.lib": "libarchive",
    "libcurl.lib": "curl",
    "bz2.lib": "bzip2",
    "lzma.lib": "xz",
    "zstd_static.lib": "zstd",
    "gumbo.lib": "gumbo",
    "yaml.lib": "yaml",
}


def classify_link_input(token: str) -> str:
    lowered = token.lower()
    if lowered.endswith(".obj"):
        return "generated-object"
    if lowered.endswith(".lib"):
        if "windows-x64-static" in lowered or "nickel-native-tools" in lowered:
            if "\\plugins\\" in lowered or "/plugins/" in lowered:
                return "static-qt-plugin-library"
            if re.search(r"qt6[a-z0-9_]*\.lib$", lowered):
                return "static-qt-library"
            return "static-dependency-library"
        return "system-or-project-import-library"
    return "unknown"


def dependency_pins(repo: Path) -> dict:
    lock_path = repo / "scripts/native/deps-lock.json"
    document = json.loads(lock_path.read_text(encoding="utf-8"))
    return {
        "source": "scripts/native/deps-lock.json",
        "sha256": sha256_file(lock_path),
        "schema": document.get("schema"),
        "qtVersion": document.get("qtVersion"),
        "windowsRuntime": document.get("windowsRuntime"),
        "sources": document.get("sources", []),
    }


def cmake_resolutions(repo: Path) -> dict:
    path = repo / "cmake/NickelDependencies.cmake"
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(r"find_package\(\s*(\w+)\s+([0-9][^ )]*)\s+EXACT")
    rows = [{"package": name, "version": version, "exact": True}
            for name, version in pattern.findall(text)]
    return {
        "source": "cmake/NickelDependencies.cmake",
        "sha256": sha256_file(path),
        "exactVersionPins": rows,
    }


def build_sbom_inputs(repo: Path, build_ninja: Path, cache: Path,
                      version: str, commit: str, link_tokens: list[str]) -> dict:
    pins = dependency_pins(repo)
    by_name = {entry["name"]: entry for entry in pins["sources"]}
    cache_text = cache.read_text(encoding="utf-8", errors="replace")
    cache_values = {}
    for key in ("CMAKE_BUILD_TYPE", "CMAKE_MSVC_RUNTIME_LIBRARY",
                "NICKEL_BUILD_UI", "NICKEL_BUILD_TESTS",
                "CMAKE_PREFIX_PATH", "CMAKE_CXX_COMPILER"):
        match = re.search(r"^%s:[A-Z]+=(.*)$" % re.escape(key), cache_text,
                          re.MULTILINE)
        cache_values[key] = match.group(1).strip() if match else None

    qt_entry = None
    qt_license = None
    qt_modules = [by_name[name] for name in ("qtbase", "qtsvg", "qtimageformats")
                  if name in by_name]
    if qt_modules:
        licences = sorted({entry.get("license") for entry in qt_modules
                           if entry.get("license")})
        qt_license = " OR ".join(licences) if licences else None
        qt_entry = {
            "package": "qt",
            "version": pins.get("qtVersion"),
            "pinnedSources": [{"name": entry["name"],
                               "sha256": entry["sha256"],
                               "license": entry.get("license")}
                              for entry in qt_modules],
            "licenseExpression": qt_license,
            "attribution": "tracked deps-lock.json pins; exact Qt submodule "
                           "attribution of each static library is not claimed",
        }

    rows, unmapped = [], []
    build_root = build_ninja.parent
    for token in link_tokens:
        category = classify_link_input(token)
        candidate = Path(token)
        if not candidate.is_absolute():
            candidate = (build_root / token).resolve()
        exists = candidate.is_file()
        basename = PurePosixPath(token.replace("\\", "/")).name
        package = None
        version_value = None
        license_value = None
        note = None
        if category == "static-qt-library":
            package = "qt"
            version_value = pins.get("qtVersion")
            license_value = qt_license
            note = "Qt 6.8.3 static prefix"
        elif category == "static-qt-plugin-library":
            package = "qt"
            version_value = pins.get("qtVersion")
            license_value = qt_license
            note = "Qt 6.8.3 static plugin library"
        elif category == "static-dependency-library":
            name = PINNED_BY_LIBRARY.get(basename.lower())
            entry = by_name.get(name) if name else None
            if entry:
                package = entry["name"]
                version_value = entry.get("version")
                license_value = entry.get("license")
                note = "tracked deps-lock.json pin"
            else:
                note = "static library in the dependency prefix with no tracked pin"
        elif category == "generated-object":
            note = "Qt plugin resource object compiled from the static prefix"
        elif category == "system-or-project-import-library":
            note = "project or Windows SDK import library"
        if not exists:
            if category in {"static-qt-library", "static-qt-plugin-library",
                            "static-dependency-library", "generated-object"}:
                raise PackageError("Actual link input is missing: %s" % token)
            note = (note + "; resolved by the linker from the SDK/toolchain, "
                    "not hashed here") if note else (
                    "resolved by the linker from the SDK/toolchain, not hashed here")
        row = {
            "path": token.replace("\\", "/"),
            "bytes": candidate.stat().st_size if exists else None,
            "sha256": sha256_file(candidate) if exists else None,
            "category": category,
            "package": package,
            "version": version_value,
            "license": license_value,
            "note": note,
        }
        rows.append(row)
        if package is None and category != "generated-object":
            unmapped.append(basename)

    metadata = json.loads((repo / "native/metadata.json").read_text(encoding="utf-8"))
    return {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "native-source-sbom-inputs",
        "product": {
            "name": metadata.get("name"),
            "displayName": metadata.get("displayName"),
            "version": version,
            "applicationId": metadata.get("applicationId"),
            "license": metadata.get("license"),
            "metadataSource": "native/metadata.json",
            "metadataSha256": sha256_file(repo / "native/metadata.json"),
        },
        "sourceCommit": commit,
        "buildConfiguration": cache_values,
        "dependencyPins": pins,
        "cmakeExactVersionPins": cmake_resolutions(repo),
        "qt": qt_entry,
        "staticLinkInputs": rows,
        "unmappedLinkInputs": sorted(set(unmapped)),
        "limitations": [
            "Only versions and licenses present in tracked inputs are stated; "
            "null means not established.",
            "No license compatibility judgment, notice completeness or legal "
            "conclusion is made.",
            "Windows SDK import libraries are linker-resolved and not hashed.",
            "Qt library-to-submodule attribution is not claimed from tracked pins.",
        ],
    }


# ---------------------------------------------------------------------------
# SHA256SUMS and orchestration
# ---------------------------------------------------------------------------

def write_sums(path: Path, entries: list[tuple[str, str]]) -> dict:
    lines = ["%s  %s" % (digest, name) for name, digest in entries]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return {
        "path": path.name,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
        "entries": [{"name": name, "sha256": digest} for name, digest in entries],
    }


def verify_sums(path: Path, base: Path, expected: dict[str, str]) -> dict:
    rows = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, _, name = line.partition("  ")
        if len(digest) != 64 or not name:
            raise PackageError("Malformed SHA256SUMS line: %r" % line)
        if name in rows:
            raise PackageError("Duplicate SHA256SUMS entry: %s" % name)
        rows[name] = digest
    if set(rows) != set(expected):
        raise PackageError("SHA256SUMS entry set mismatch: %s"
                           % (sorted(set(rows) ^ set(expected))))
    checked = []
    for name, digest in rows.items():
        target = base / name
        if target.exists():
            actual = sha256_file(target)
        else:
            actual = expected[name]
        if actual != digest:
            raise PackageError("SHA256SUMS hash mismatch for %s" % name)
        checked.append({"name": name, "sha256": digest, "onDiskVerified":
                        target.exists()})
    return {"path": path.name, "sha256": sha256_file(path),
            "entries": sorted(checked, key=lambda row: row["name"])}


def command_build(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    commit = args.commit or run_git(repo, "rev-parse", "HEAD").strip()

    installed = Path(args.installed_dir).resolve()
    rows = installed_program_files(installed, args.executable)
    top = args.top_level or "pd-launcher-windows-x64-%s" % args.version
    program_zip = output / ("%s.zip" % top)
    package = build_program_zip(installed, rows, program_zip, top)

    source_top = "pd-launcher-%s-source" % args.version
    source_zip = output / ("%s.zip" % source_top)
    source = create_source_archive(repo, commit, source_zip, source_top)
    source_check = verify_source_archive(repo, source["commit"], source_zip,
                                        source["prefix"])
    if source_check["sha256"] != source["sha256"]:
        raise PackageError("Source archive changed between creation and verification")

    link_tokens = link_inputs(Path(args.build_ninja), args.target)
    sbom = build_sbom_inputs(repo, Path(args.build_ninja), Path(args.cmake_cache),
                             args.version, source["commit"], link_tokens)
    sbom_path = output / "sbom-inputs.json"
    sbom_path.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8",
                         newline="\n")

    sums_entries = [(package["path"], package["sha256"])]
    sums_entries += [(row["archivePath"], row["sha256"]) for row in package["members"]]
    sums_entries += [(source["path"], source["sha256"])]
    sums = write_sums(output / "SHA256SUMS", sums_entries)

    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "native-windows-program-package",
        "releaseClaim": False,
        "product": sbom["product"],
        "sourceCommit": source["commit"],
        "configuration": "Release",
        "platform": "windows",
        "architecture": "x64",
        "programPackage": package,
        "sourceArchive": source,
        "sha256sums": sums,
        "sbomInputs": {
            "path": sbom_path.name,
            "sha256": sha256_file(sbom_path),
            "staticLinkInputCount": len(sbom["staticLinkInputs"]),
        },
        "verification": {
            "programZipMembersByteEqualInstalled": True,
            "sourceArchiveMemberSetEqualsCommitTree": True,
            "sourceArchiveMemberContentEqualsCommitBlobs": True,
            "sourceArchiveCrlfExportedMemberCount":
                source_check["crlfExportedMemberCount"],
            "sourceArchiveUntrackedMembers": source_check["untrackedMembers"],
            "sourceArchiveSuspiciousTrackedArtifactCount":
                len(source_check["suspiciousTrackedArtifacts"]),
            "sourceArchiveSuspiciousTrackedArtifacts":
                source_check["suspiciousTrackedArtifacts"],
        },
        "gaps": [
            "No clean-machine or freshly imaged Windows host run.",
            "No Authenticode signature or timestamp.",
            "No macOS or Linux package.",
            "No legal review of licenses, notices or AGPL source-offer terms.",
        ],
    }
    manifest_path = output / "package-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             encoding="utf-8", newline="\n")
    print(json.dumps({
        "ok": True,
        "outputDir": str(output),
        "programZip": {"path": str(program_zip), "bytes": package["bytes"],
                       "sha256": package["sha256"]},
        "sourceArchive": {"path": str(source_zip), "bytes": source["bytes"],
                          "sha256": source["sha256"]},
        "sha256sums": {"path": str(output / "SHA256SUMS"),
                       "sha256": sums["sha256"]},
        "sbomInputs": {"path": str(sbom_path),
                       "sha256": manifest["sbomInputs"]["sha256"]},
        "manifest": {"path": str(manifest_path),
                     "sha256": sha256_file(manifest_path)},
        "sourceCommit": source["commit"],
        "programMembers": [row["distributionPath"] for row in package["members"]],
    }, indent=2))
    return 0


def command_verify(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    installed = Path(args.installed_dir).resolve()
    output = Path(args.output_dir).resolve()
    manifest = json.loads((output / "package-manifest.json").read_text(encoding="utf-8"))
    package = manifest["programPackage"]
    source = manifest["sourceArchive"]
    zip_path = Path(args.zip) if args.zip else output / package["path"]
    source_zip = (Path(args.source_zip) if args.source_zip
                  else output / source["path"])
    sums_path = Path(args.sums) if args.sums else output / "SHA256SUMS"

    members = verify_program_zip(zip_path, installed, package["topLevelDirectory"])
    if sha256_file(zip_path) != package["sha256"]:
        raise PackageError("Program ZIP hash differs from the recorded manifest")
    if zip_path.stat().st_size != package["bytes"]:
        raise PackageError("Program ZIP size differs from the recorded manifest")
    if [{key: row[key] for key in ("distributionPath", "archivePath", "bytes",
                                   "sha256")} for row in members] != package["members"]:
        raise PackageError("Program ZIP member inventory differs from the manifest")

    source_result = verify_source_archive(repo, source["commit"], source_zip,
                                          source["prefix"])
    if source_result["sha256"] != source["sha256"]:
        raise PackageError("Source archive hash differs from the recorded manifest")

    expected = {package["path"]: package["sha256"]}
    expected.update({row["archivePath"]: row["sha256"] for row in members})
    expected[source["path"]] = source["sha256"]
    sums = verify_sums(sums_path, output, expected)

    print(json.dumps({
        "ok": True,
        "programZip": {"path": str(zip_path), "bytes": zip_path.stat().st_size,
                       "sha256": sha256_file(zip_path),
                       "members": members},
        "sourceArchive": source_result,
        "sha256sums": sums,
        "rejectedExtras": [],
        "cleanMachineTested": False,
        "signed": False,
    }, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="create program/source/SBOM package")
    build.add_argument("--repo", default=".")
    build.add_argument("--installed-dir", required=True)
    build.add_argument("--output-dir", required=True)
    build.add_argument("--version", required=True)
    build.add_argument("--commit")
    build.add_argument("--top-level")
    build.add_argument("--executable", default="pd-launcher.exe")
    build.add_argument("--build-ninja", required=True)
    build.add_argument("--cmake-cache", required=True)
    build.add_argument("--target", default="pd-launcher.exe")
    build.set_defaults(func=command_build)

    verify = sub.add_parser("verify", help="independently re-verify a package")
    verify.add_argument("--repo", default=".")
    verify.add_argument("--installed-dir", required=True)
    verify.add_argument("--output-dir", required=True)
    verify.add_argument("--zip")
    verify.add_argument("--source-zip")
    verify.add_argument("--sums")
    verify.set_defaults(func=command_verify)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except PackageError as error:
        print(json.dumps({"ok": False, "error": str(error)}, indent=2),
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
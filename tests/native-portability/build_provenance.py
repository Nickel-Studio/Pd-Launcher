#!/usr/bin/env python3
"""Bind an owned build cache and audit inputs to the exact source identity."""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path
import posixpath
import re

from audit_support import AuditSafetyError, OWNED, atomic_json, owned_output, plain_path, sha256

PROVENANCE_NAME = ".nickel-portability-build.json"
AUDIT_INPUTS = ("CMakeLists.txt", "portability_probe.cpp", "core_link_probe.cpp", "probe.rc", "probe.manifest")
HASH = re.compile(r"[0-9a-f]{64}\Z")


def bounded_object(path: Path, limit: int = 65536) -> dict:
    path = plain_path(path, must_exist=True)
    if not path.is_file() or path.stat().st_size > limit:
        raise AuditSafetyError("Audit identity must be a bounded regular JSON file: " + str(path))
    value = json.loads(path.read_bytes())
    if not isinstance(value, dict):
        raise AuditSafetyError("Audit identity must be a JSON object")
    return value


def audit_inputs() -> dict:
    return {name: sha256(plain_path(OWNED / name, root=OWNED, must_exist=True)) for name in AUDIT_INPUTS}


def build_identity(build: Path, root: Path, native_cache_path: str, audit_cache_path: str) -> dict:
    build = owned_output(build)
    root = plain_path(root, must_exist=True)
    if not build.is_dir() or not root.is_dir():
        raise AuditSafetyError("Exact owned build and native directories must exist")
    marker = bounded_object(build.parent / ".nickel-portability-owned.json", 4096)
    owner = marker.get("owner")
    if not isinstance(owner, str) or not Path(owner).is_absolute() or plain_path(Path(owner)) != OWNED:
        raise AuditSafetyError("Build owner marker does not match this exact audit owner")
    cache = plain_path(build / "CMakeCache.txt", root=build, must_exist=True)
    if not cache.is_file() or cache.stat().st_size > 4 * 1024 * 1024:
        raise AuditSafetyError("Build cache must be a bounded regular file")
    entries = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        key_type, separator, value = line.partition("=")
        key, typed, _ = key_type.partition(":")
        if separator and typed:
            if key in entries:
                raise AuditSafetyError("Duplicate CMake cache identity: " + key)
            entries[key] = value
    expected = {"NICKEL_NATIVE_ROOT": native_cache_path, "CMAKE_HOME_DIRECTORY": audit_cache_path,
                "CMAKE_BUILD_TYPE": "Release", "CMAKE_GENERATOR": "Ninja"}
    if any(entries.get(key) != value for key, value in expected.items()):
        raise AuditSafetyError("Existing build cache is not this exact source, Release configuration and Ninja generator")
    override = entries.get("NICKEL_INSTANCES_TOML_INCLUDE_DIR", "")
    if posixpath.normpath(override) == "/usr/include/toml++":
        raise AuditSafetyError("Historical toml++ header-layout workaround cannot be resumed as a default build")
    return {"schema": 1, "kind": "owned-portability-build-provenance", "owner": str(OWNED),
            "buildPhysicalPath": str(build), "nativePhysicalPath": str(root),
            "cacheSha256": sha256(cache), "auditInputs": audit_inputs()}


def record(build: Path, root: Path, source_sha256: str, manifest_sha256: str | None,
           native_cache_path: str, audit_cache_path: str) -> Path:
    if not HASH.fullmatch(source_sha256) or (manifest_sha256 is not None and not HASH.fullmatch(manifest_sha256)):
        raise AuditSafetyError("Build provenance requires exact SHA-256 identities")
    identity = build_identity(build, root, native_cache_path, audit_cache_path)
    path = owned_output(build / PROVENANCE_NAME)
    if path.exists():
        raise AuditSafetyError("Refusing to replace an existing build provenance record")
    identity.update(sourceInventorySha256=source_sha256, manifestSha256=manifest_sha256,
                    createdUtc=dt.datetime.now(dt.timezone.utc).isoformat(), completeProductVerified=False)
    atomic_json(path, identity)
    return path


def validate(build: Path, root: Path, source_sha256: str, manifest_sha256: str,
             native_cache_path: str, audit_cache_path: str) -> dict:
    if not HASH.fullmatch(source_sha256) or not HASH.fullmatch(manifest_sha256):
        raise AuditSafetyError("Resume requires exact source and frozen manifest SHA-256 identities")
    current = build_identity(build, root, native_cache_path, audit_cache_path)
    current.update(sourceInventorySha256=source_sha256, manifestSha256=manifest_sha256)
    path = owned_output(build / PROVENANCE_NAME)
    recorded = bounded_object(path)
    if any(recorded.get(key) != value for key, value in current.items()):
        raise AuditSafetyError("Build provenance differs from the current snapshot, cache or audit inputs; start a fresh owned build")
    return {"path": str(path), "sha256": sha256(path), "verified": True,
            "scope": "Build-input identity only; not compilation, runtime or complete-product acceptance"}

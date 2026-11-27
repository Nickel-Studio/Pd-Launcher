#!/usr/bin/env python3
"""Generate deterministic compiled notice material for the Windows production EXE.

Reads the verified trusted-launcher notice inputs (never mutating them),
recomputes every size and SHA256, reproduces the canonical notice-set digest,
and emits three build inputs into an untracked build directory:

    notice_resources.rc     one Win32 RCDATA resource per notice + the manifest
    TrustedLauncherBuild.hpp  metadata consumed by app/service/trusted_launcher.cpp
    notice-manifest.json    compact, self-describing resource map embedded as RCDATA

The mapping is content-defined: notices are sorted by name and numbered
``PD_NOTICE_000`` ... in that order, so the generated files are a pure function
of the manifest and notice bytes (no timestamps, no host paths inside the EXE).

Refuses to run on any mismatch: the build must fail closed rather than embed
unverified or unsafe material.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from legal_input_verify import (  # noqa: E402 - local verification helpers
    notice_set_digest,
    sha256_bytes,
    sha256_file,
    unsafe_reason,
)

MANIFEST = "docs/native/licenses/trusted-launcher-notice-input.json"
NOTICE_ROOT = "docs/native/licenses/trusted-launcher-notice-inputs"
REQUIRED_PRIMARY = ("LICENSE", "LICENSE-ASSETS", "NOTICE.md", "SOURCE.txt")

# Mirror the runtime consumer limits in app/service/trusted_launcher.cpp.
MAX_NOTICES = 512
MAX_TOTAL_BYTES = 16 * 1024 * 1024
MAX_NOTICE_BYTES = 4 * 1024 * 1024
MAX_NAME_BYTES = 4096

RESOURCE_PREFIX = "PD_NOTICE_"
MANIFEST_RESOURCE = "PD_NOTICE_MANIFEST"
RESOURCE_TYPE = "RCDATA"

SCHEMA_VERSION = 1


class Failure(RuntimeError):
    pass


def fail(message: str) -> None:
    raise Failure(message)


def cpp_string(value: str) -> str:
    """Emit a C++ string literal without ambiguity between escapes and digits."""
    out = ['"']
    for byte in value.encode("utf-8"):
        if 0x20 <= byte <= 0x7E and byte not in (0x22, 0x5C):
            out.append(chr(byte))
        else:
            # Always three octal digits; a following character can never extend it.
            out.append("\\%03o" % byte)
    out.append('"')
    return "".join(out)


def rc_string_path(path: Path) -> str:
    text = path.resolve().as_posix()
    if '"' in text or any(ord(character) < 0x20 for character in text):
        fail(f"resource path is not RC-safe: {text!r}")
    return text


def verify_inputs(repo: Path, manifest_path: Path) -> tuple[dict, list[dict], bytes]:
    manifest_bytes = manifest_path.read_bytes()
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"manifest is not UTF-8 JSON: {error}")

    rows = manifest.get("notices")
    if not isinstance(rows, list) or not rows:
        fail("manifest has no notice rows")
    if manifest.get("noticeCount") != len(rows):
        fail(f"noticeCount {manifest.get('noticeCount')} != {len(rows)} rows")
    if len(rows) > MAX_NOTICES:
        fail(f"{len(rows)} notices exceed the runtime limit {MAX_NOTICES}")

    casefold: dict[str, str] = {}
    total = 0
    verified: list[dict] = []
    for row in rows:
        name = row.get("name")
        if not isinstance(name, str):
            fail(f"notice row without a string name: {row!r}")
        reason = unsafe_reason(name)
        if reason:
            fail(f"unsafe notice name {name!r}: {reason}")
        if len(name.encode("utf-8")) > MAX_NAME_BYTES:
            fail(f"notice name longer than {MAX_NAME_BYTES} bytes: {name!r}")
        folded = name.casefold()
        if folded in casefold:
            fail(f"duplicate case-insensitive notice name: {name!r} vs {casefold[folded]!r}")
        casefold[folded] = name

        recorded_path = row.get("path")
        expected_path = f"{NOTICE_ROOT}/{name}"
        if recorded_path != expected_path:
            fail(f"recorded path mismatch for {name!r}: {recorded_path!r}")
        target = repo / expected_path
        if not target.is_file():
            fail(f"missing notice file: {expected_path}")
        if target.is_symlink():
            fail(f"notice input must not be a symlink: {expected_path}")

        size, digest = sha256_file(target)
        if size != row.get("bytes"):
            fail(f"size mismatch for {name!r}: {row.get('bytes')} != {size}")
        if digest != row.get("sha256"):
            fail(f"SHA256 mismatch for {name!r}: {row.get('sha256')} != {digest}")
        if size == 0 or size > MAX_NOTICE_BYTES:
            fail(f"notice {name!r} size {size} outside 1..{MAX_NOTICE_BYTES}")
        if total > MAX_TOTAL_BYTES - size:
            fail(f"notice payload total exceeds {MAX_TOTAL_BYTES} bytes")
        payload = target.read_bytes()
        if len(payload) != size:
            fail(f"short read for {name!r}")
        if b"\0" in payload:
            fail(f"notice {name!r} contains a NUL byte")
        total += size
        verified.append(
            {
                "name": name,
                "bytes": size,
                "sha256": digest,
                "path": expected_path,
                "payload": payload,
            }
        )

    if total != manifest.get("noticeBytes"):
        fail(f"total notice bytes {total} != manifest noticeBytes {manifest.get('noticeBytes')}")
    present_names = {row["name"] for row in verified}
    for primary in REQUIRED_PRIMARY:
        if primary not in present_names:
            fail(f"required primary notice is missing: {primary}")

    canonical = notice_set_digest(rows, "name/bytes/sha256")
    if canonical != manifest.get("notice_set_sha256"):
        fail(f"notice_set_sha256 not reproduced: {canonical} != {manifest.get('notice_set_sha256')}")
    if notice_set_digest(rows, "name") != canonical:
        fail("notice-set digest differs between the two documented row orders")

    expected_names = {row["name"] for row in rows} | {".gitattributes"}
    disk = sorted(
        str(path.relative_to(repo / NOTICE_ROOT)).replace("\\", "/")
        for path in (repo / NOTICE_ROOT).rglob("*")
        if path.is_file()
    )
    for name in disk:
        if name not in expected_names:
            fail(f"unexpected file under the notice input root: {name!r}")
    if len(disk) != len(expected_names):
        fail(f"notice input root has {len(disk)} files, expected {len(expected_names)}")

    repository = manifest.get("repository")
    if not isinstance(repository, str) or not repository.startswith("https://"):
        fail("manifest repository must be an https URL")

    verified.sort(key=lambda row: row["name"].encode("utf-8"))
    identity = {
        "schemaVersion": SCHEMA_VERSION,
        "kind": "pd-compiled-launcher-notices",
        "sourceManifest": MANIFEST,
        "sourceManifestBytes": len(manifest_bytes),
        "sourceManifestSha256": sha256_bytes(manifest_bytes),
        "repository": repository,
        "noticeCount": len(verified),
        "noticeBytes": total,
        "noticeSetSha256": canonical,
        "noticeSetHashEncoding": manifest.get("noticeSetHashEncoding"),
        "resourceType": RESOURCE_TYPE,
        "resourcePrefix": RESOURCE_PREFIX,
        "manifestResource": MANIFEST_RESOURCE,
        "sourceManifestFlags": {
            key: manifest.get(key)
            for key in (
                "compiledNoticeBytesVerified",
                "applicationSourceAndObjectsBound",
                "full40LibraryLicenseSourceBuildBindingVerified",
                "trusted_launcher_ready",
                "nativeLegalMaterialValidationPassed",
                "nativeReleaseReady",
            )
        },
    }
    return identity, verified, manifest_bytes


def emit_manifest(identity: dict, notices: list[dict]) -> bytes:
    payload = dict(identity)
    payload["notices"] = [
        {
            "name": row["name"],
            "resource": row["resource"],
            "bytes": row["bytes"],
            "sha256": row["sha256"],
        }
        for row in notices
    ]
    text = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return text.encode("ascii")


def emit_header(identity: dict, notices: list[dict], manifest_digest: str) -> str:
    lines = [
        "#pragma once",
        "// Generated by scripts/native/generate_notice_resources.py. Do not edit.",
        f"// Manifest: {identity['sourceManifest']}",
        f"// Manifest SHA256: {identity['sourceManifestSha256']}",
        f"// Notice set SHA256: {identity['noticeSetSha256']}",
        f"// Embedded manifest resource: {MANIFEST_RESOURCE} ({manifest_digest})",
        '#include "app/service/trusted_launcher.hpp"',
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace nickel::app::build {",
        "",
        "// Notice bytes are compiled, but release readiness is not granted here:",
        "// source/object binding, the 40-library binding and signing are separate gates.",
        "inline constexpr bool trusted_launcher_ready = false;",
        f"inline constexpr std::string_view trusted_launcher_manifest_sha256 = {cpp_string(identity['sourceManifestSha256'])};",
        f"inline constexpr std::string_view trusted_launcher_notice_set_sha256 = {cpp_string(identity['noticeSetSha256'])};",
        'inline constexpr std::string_view trusted_launcher_source_set_sha256 = "";',
        f"inline constexpr std::string_view trusted_launcher_repository = {cpp_string(identity['repository'])};",
        'inline constexpr std::string_view trusted_launcher_stamp_resource = "";',
        f"inline constexpr std::uint64_t trusted_launcher_notice_count = {identity['noticeCount']};",
        f"inline constexpr std::uint64_t trusted_launcher_notice_bytes = {identity['noticeBytes']};",
        "",
        f"inline constexpr std::array<EmbeddedLauncherNotice, {len(notices)}> trusted_launcher_notices{{{{",
    ]
    for row in notices:
        lines.append(
            "    EmbeddedLauncherNotice{"
            + cpp_string(row["name"])
            + ", {}, "
            + cpp_string(row["sha256"])
            + ", "
            + cpp_string(row["resource"])
            + ", "
            + str(row["bytes"])
            + "},"
        )
    lines += ["}};", "", "} // namespace nickel::app::build", ""]
    return "\n".join(lines)


def emit_rc(identity: dict, notices: list[dict], manifest_path: Path, payload_digest: str) -> str:
    lines = [
        "// Generated by scripts/native/generate_notice_resources.py. Do not edit.",
        f"// Payload set SHA256: {payload_digest}",
        f"// Notice set SHA256: {identity['noticeSetSha256']}",
        "#include <winres.h>",
        "",
        f"{MANIFEST_RESOURCE} RCDATA {cpp_string(rc_string_path(manifest_path))}",
        "",
    ]
    for row in notices:
        lines.append(f"{row['resource']} RCDATA {cpp_string(rc_string_path(row['target']))}")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--manifest", default=MANIFEST)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--json")
    options = parser.parse_args(argv)

    repo = Path(options.repo).resolve()
    manifest_path = Path(options.manifest)
    if not manifest_path.is_absolute():
        manifest_path = repo / manifest_path
    out_dir = Path(options.out_dir).resolve()

    try:
        identity, notices, _ = verify_inputs(repo, manifest_path)
        for index, row in enumerate(notices):
            row["resource"] = f"{RESOURCE_PREFIX}{index:03d}"
            row["target"] = repo / row["path"]
        resource_names = {row["resource"] for row in notices}
        if len(resource_names) != len(notices):
            fail("duplicate generated resource names")

        out_dir.mkdir(parents=True, exist_ok=True)
        manifest_output = out_dir / "notice-manifest.json"
        header_output = out_dir / "TrustedLauncherBuild.hpp"
        rc_output = out_dir / "notice_resources.rc"

        manifest_bytes = emit_manifest(identity, notices)
        manifest_digest = sha256_bytes(manifest_bytes)
        payload_digest = sha256_bytes(
            b"".join(row["payload"] for row in notices)
            + f"{identity['noticeSetSha256']}\n".encode("ascii")
        )

        manifest_output.write_bytes(manifest_bytes)
        header_output.write_text(emit_header(identity, notices, manifest_digest), encoding="utf-8", newline="\n")
        rc_output.write_text(
            emit_rc(identity, notices, manifest_output, payload_digest), encoding="utf-8", newline="\n"
        )

        result = {
            "status": "PASS",
            "kind": identity["kind"],
            "repo": str(repo),
            "outDir": str(out_dir),
            "sourceManifestSha256": identity["sourceManifestSha256"],
            "noticeCount": identity["noticeCount"],
            "noticeBytes": identity["noticeBytes"],
            "noticeSetSha256": identity["noticeSetSha256"],
            "resourceType": RESOURCE_TYPE,
            "resourceEntries": len(notices) + 1,
            "manifestResource": MANIFEST_RESOURCE,
            "manifestBytes": len(manifest_bytes),
            "manifestSha256": manifest_digest,
            "payloadSetSha256": payload_digest,
            "outputs": {
                path.name: {"bytes": path.stat().st_size, "sha256": sha256_file(path)[1]}
                for path in (rc_output, header_output, manifest_output)
            },
            "notices": [
                {"name": row["name"], "resource": row["resource"], "bytes": row["bytes"], "sha256": row["sha256"]}
                for row in notices
            ],
        }
    except Failure as error:
        print(json.dumps({"status": "FAIL", "reason": str(error)}, indent=1), file=sys.stderr)
        return 1

    if options.json:
        Path(options.json).parent.mkdir(parents=True, exist_ok=True)
        Path(options.json).write_text(json.dumps(result, indent=1, ensure_ascii=True), encoding="utf-8")
    print(
        json.dumps(
            {
                "status": result["status"],
                "noticeCount": result["noticeCount"],
                "noticeBytes": result["noticeBytes"],
                "resourceEntries": result["resourceEntries"],
                "noticeSetSha256": result["noticeSetSha256"],
                "outputs": result["outputs"],
            },
            indent=1,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
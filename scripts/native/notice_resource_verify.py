#!/usr/bin/env python3
"""Independently verify compiled notice RCDATA inside a Windows PE.

The check is deliberately independent of the builder: it walks the PE resource
directory itself, re-slices every payload from the file, recomputes SHA256, and
compares it with both the generated resource map and the original notice inputs.
It never trusts a hash recorded by the generator.

Checks performed
    * every generated resource name exists exactly once as RCDATA
    * every payload length and SHA256 matches the generated map
    * every generated map row matches the original notice input bytes
    * the embedded PD_NOTICE_MANIFEST is byte-identical to the generated map
    * notice_set_sha256 is reproduced from the original inputs and matches
    * all original notice payloads are byte-identical substrings of the EXE
    * optional baseline comparison for EXE size and resource totals

Usage
    python -B scripts/native/notice_resource_verify.py \
        --exe <pd-launcher.exe> \
        --generated-manifest <build>/native/app/generated/notice-manifest.json \
        --baseline <pre-embedding.exe> --json <out.json>
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from legal_input_verify import (  # noqa: E402 - shared read-only helpers
    MANIFEST,
    NOTICE_ROOT,
    notice_set_digest,
    pe_resources,
    sha256_bytes,
    sha256_file,
)

MANIFEST_RESOURCE = "PD_NOTICE_MANIFEST"


def payload_of(data: bytes, entry: dict) -> bytes:
    offset = entry.get("payload_offset")
    size = entry.get("bytes", 0)
    if offset is None or offset < 0 or offset + size > len(data):
        return b""
    return data[offset:offset + size]


def resource_summary(resources: dict) -> dict:
    summary: dict[str, dict[str, int]] = {}
    for entry in resources["entries"]:
        kind = str(entry["type"])
        summary.setdefault(kind, {"entries": 0, "bytes": 0})
        summary[kind]["entries"] += 1
        summary[kind]["bytes"] += entry["bytes"]
    return summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--generated-manifest", required=True)
    parser.add_argument("--repo", default=".")
    parser.add_argument("--source-manifest", default=MANIFEST)
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--json")
    options = parser.parse_args(argv)

    repo = Path(options.repo).resolve()
    exe = Path(options.exe).resolve()
    data = exe.read_bytes()
    findings: list[str] = []
    failures: list[str] = []
    checks: dict[str, object] = {}

    checks["exe"] = {"path": str(exe), "bytes": len(data), "sha256": sha256_bytes(data)}

    resources = pe_resources(data)
    entries = resources["entries"]
    rcdata = [entry for entry in entries if entry["type"] == "RCDATA"]
    summary = resource_summary(resources)
    checks["resource_types"] = summary
    checks["rcdata_entry_count"] = len(rcdata)
    checks["rcdata_bytes_total"] = sum(entry["bytes"] for entry in rcdata)
    findings.append(
        "PE resources: "
        + ", ".join(f"{kind}x{value['entries']}/{value['bytes']}B" for kind, value in sorted(summary.items()))
    )

    named: dict[str, dict] = {}
    for entry in rcdata:
        # PE leaf ids repeat (language id); the stable resource name is path[1].
        path = entry.get("path") or []
        name = path[1] if len(path) >= 3 else entry.get("id")
        if not isinstance(name, str):
            failures.append(f"RCDATA entry is not a named resource: {path}")
            continue
        if name in named:
            failures.append(f"duplicate RCDATA resource name: {name}")
        named[name] = entry
    checks["rcdata_names"] = sorted(named)

    generated_path = Path(options.generated_manifest).resolve()
    generated_bytes = generated_path.read_bytes()
    generated = json.loads(generated_bytes.decode("ascii"))
    checks["generated_manifest"] = {
        "path": str(generated_path),
        "bytes": len(generated_bytes),
        "sha256": sha256_bytes(generated_bytes),
    }

    if MANIFEST_RESOURCE not in named:
        failures.append(f"missing embedded resource {MANIFEST_RESOURCE}")
    else:
        embedded = payload_of(data, named[MANIFEST_RESOURCE])
        checks["embedded_manifest_bytes"] = len(embedded)
        checks["embedded_manifest_sha256"] = sha256_bytes(embedded)
        if embedded != generated_bytes:
            failures.append("embedded manifest differs from the generated manifest")
        else:
            findings.append(
                f"embedded {MANIFEST_RESOURCE} is byte-identical to the generated map ({len(embedded)} B)"
            )

    source_manifest_path = Path(options.source_manifest)
    if not source_manifest_path.is_absolute():
        source_manifest_path = repo / source_manifest_path
    source_bytes = source_manifest_path.read_bytes()
    source = json.loads(source_bytes.decode("utf-8"))
    source_rows = source["notices"]
    recomputed_set = notice_set_digest(source_rows, "name/bytes/sha256")
    checks["source_manifest"] = {
        "path": str(source_manifest_path),
        "bytes": len(source_bytes),
        "sha256": sha256_bytes(source_bytes),
        "noticeCount": len(source_rows),
        "noticeBytes": sum(int(row["bytes"]) for row in source_rows),
        "notice_set_sha256_recomputed": recomputed_set,
    }
    if generated.get("sourceManifestSha256") != sha256_bytes(source_bytes):
        failures.append("generated map sourceManifestSha256 does not describe the input manifest")
    if generated.get("noticeSetSha256") != recomputed_set:
        failures.append("generated map noticeSetSha256 differs from the recomputed input digest")
    if generated.get("noticeCount") != len(source_rows):
        failures.append("generated map noticeCount differs from the input manifest")
    if generated.get("noticeBytes") != sum(int(row["bytes"]) for row in source_rows):
        failures.append("generated map noticeBytes differs from the input manifest")
    if recomputed_set != source.get("notice_set_sha256"):
        failures.append("notice_set_sha256 not reproduced from the input manifest rows")

    rows = generated.get("notices", [])
    expected_names = {row["resource"] for row in rows}
    extra = sorted(set(named) - expected_names - {MANIFEST_RESOURCE})
    missing = sorted(expected_names - set(named))
    checks["missing_resources"] = missing
    checks["unexpected_resources"] = extra
    if missing:
        failures.append(f"missing RCDATA notice resources: {missing[:10]}")
    if extra:
        failures.append(f"unexpected RCDATA resources: {extra[:10]}")

    verified = 0
    byte_mismatch: list[str] = []
    hash_mismatch: list[str] = []
    input_mismatch: list[str] = []
    total = 0
    for row in rows:
        entry = named.get(row["resource"])
        if entry is None:
            continue
        payload = payload_of(data, entry)
        if len(payload) != int(row["bytes"]) or len(payload) != entry["bytes"]:
            byte_mismatch.append(row["name"])
            continue
        digest = sha256_bytes(payload)
        if digest != row["sha256"] or entry.get("sha256") != row["sha256"]:
            hash_mismatch.append(row["name"])
            continue
        original = repo / NOTICE_ROOT / row["name"]
        if not original.is_file():
            input_mismatch.append(row["name"])
            continue
        size, original_digest = sha256_file(original)
        if size != int(row["bytes"]) or original_digest != row["sha256"]:
            input_mismatch.append(row["name"])
            continue
        total += len(payload)
        verified += 1

    checks["notice_payloads_verified"] = verified
    checks["notice_payloads_total_bytes"] = total
    checks["notice_payload_size_mismatch"] = byte_mismatch
    checks["notice_payload_sha256_mismatch"] = hash_mismatch
    checks["notice_input_mismatch"] = input_mismatch
    if byte_mismatch:
        failures.append(f"payload size mismatches: {byte_mismatch[:10]}")
    if hash_mismatch:
        failures.append(f"payload SHA256 mismatches: {hash_mismatch[:10]}")
    if input_mismatch:
        failures.append(f"payload differs from the original input: {input_mismatch[:10]}")
    findings.append(f"RCDATA notice payloads verified {verified}/{len(rows)} with exact bytes and SHA256")
    if verified != len(rows) or len(rows) != len(source_rows):
        failures.append(f"verified {verified}/{len(rows)} generated rows against {len(source_rows)} inputs")

    absent = [
        row["name"]
        for row in source_rows
        if (repo / NOTICE_ROOT / row["name"]).read_bytes() not in data
    ]
    checks["notice_payloads_absent_from_exe"] = absent
    findings.append(f"{len(source_rows) - len(absent)}/{len(source_rows)} original notices are byte-identical EXE substrings")
    if absent:
        failures.append(f"notice payloads absent from the EXE: {absent[:10]}")

    if options.baseline:
        baseline = Path(options.baseline).resolve()
        baseline_data = baseline.read_bytes()
        baseline_resources = pe_resources(baseline_data)
        baseline_summary = resource_summary(baseline_resources)
        checks["baseline"] = {
            "path": str(baseline),
            "bytes": len(baseline_data),
            "sha256": sha256_bytes(baseline_data),
            "resource_types": baseline_summary,
            "rcdata_entry_count": sum(1 for entry in baseline_resources["entries"] if entry["type"] == "RCDATA"),
        }
        checks["size_delta_bytes"] = len(data) - len(baseline_data)
        findings.append(
            f"EXE size {len(baseline_data)} -> {len(data)} B (delta {len(data) - len(baseline_data):+d})"
        )
        if baseline_summary.get("RCDATA", {}).get("entries", 0) != 0:
            findings.append("NOTE baseline already had RCDATA entries; delta includes prior content")

    status = "PASS" if not failures else "FAIL"
    result = {
        "check": "notice-resources",
        "status": status,
        "findings": findings,
        "failures": failures,
        "checks": checks,
    }
    print(json.dumps(result, indent=1, ensure_ascii=False, default=str))
    if options.json:
        Path(options.json).parent.mkdir(parents=True, exist_ok=True)
        Path(options.json).write_text(
            json.dumps(result, indent=1, ensure_ascii=False, default=str), encoding="utf-8"
        )
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
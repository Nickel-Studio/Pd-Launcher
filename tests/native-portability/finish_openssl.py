#!/usr/bin/env python3
"""Finish the actual pinned OpenSSL source with a fresh build and retained failed provenance."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tarfile

from audit_support import AuditSafetyError, OWNED, owned_output, plain_path, sha256
from linux_d_sdk import DriveSdk, OPENSSL_SIGNING_FINGERPRINT, load_openssl_pin, verify_gpg_status
from verify_snapshot import HASH, relative_key, records_hash


def source_check(archive: Path, source: Path) -> dict:
    rows = {}
    with tarfile.open(archive) as bundle:
        for member in bundle:
            name = member.name.rstrip("/") if member.isdir() else member.name
            key = relative_key(name).as_posix()
            if not key.startswith(source.name + "/") and key != source.name:
                raise AuditSafetyError("OpenSSL archive has an unexpected source root")
            if member.isdir():
                continue
            if not member.isfile():
                raise AuditSafetyError("OpenSSL source verification rejects special archive members")
            relative = key[len(source.name) + 1:]
            if not relative or relative in rows:
                raise AuditSafetyError("OpenSSL archive contains an empty or duplicate file key")
            path = source / relative
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_size != member.size:
                raise AuditSafetyError("Retained OpenSSL source type or size changed: " + relative)
            with bundle.extractfile(member) as stream:
                expected = hashlib.file_digest(stream, "sha256").hexdigest()
            if sha256(path) != expected:
                raise AuditSafetyError("Retained OpenSSL source differs from its archive: " + relative)
            rows[relative] = {"bytes": member.size, "sha256": expected}
    actual = set()
    for directory, folders, files in os.walk(source, followlinks=False):
        for name in folders:
            if not stat.S_ISDIR((Path(directory) / name).lstat().st_mode):
                raise AuditSafetyError("Retained source contains a symlink or special directory")
        for name in files:
            path = Path(directory) / name
            actual.add(path.relative_to(source).as_posix())
    if actual != set(rows):
        raise AuditSafetyError("Retained OpenSSL source has unlisted or missing files")
    return {"fileCount": len(rows), "sourceBytes": sum(row["bytes"] for row in rows.values()),
            "sourceSetSha256": records_hash(rows), "archiveBytesVerified": True}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--previous-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resume-build", action="store_true")
    args = parser.parse_args()
    sdk = None
    try:
        previous = owned_output(args.previous)
        if not HASH.fullmatch(args.previous_sha256) or sha256(previous) != args.previous_sha256:
            raise AuditSafetyError("Explicit unchanged failed OpenSSL phase required")
        prior = json.loads(previous.read_bytes())
        ssl, signature = prior["openssl"], prior["opensslSignature"]
        if (prior.get("state") != "FAILED_OR_BLOCKED" or prior.get("passed") is not False
                or ssl.get("signatureVerified") is not True or ssl.get("systemSslUsed") is not False
                or signature.get("cryptographicSignatureVerified") is not True
                or signature.get("primaryFingerprint") != OPENSSL_SIGNING_FINGERPRINT):
            raise AuditSafetyError("The retained phase lacks a verified pinned source")
        qt, freeze = prior["retainedQt"], prior["dependencySource"]
        sdk = DriveSdk(Path(prior["root"]), prior["token"], args.output, Path(qt["ownershipEvidence"]),
                       Path(qt["supplyEvidence"]), qt["supplyEvidenceSha256"],
                       Path(freeze["manifestPath"]), freeze["manifestSha256"])
        if prior["markerSha256"] != hashlib.sha256(sdk.marker_bytes).hexdigest():
            raise AuditSafetyError("OpenSSL continuation root marker differs")
        pin_path = OWNED / "openssl-linux-pin.json"
        pin = load_openssl_pin(pin_path)
        archive = plain_path(Path(ssl["archivePath"]), root=sdk.root / "downloads", must_exist=True)
        source = plain_path(Path(ssl["sourcePath"]), root=sdk.root / "sources", must_exist=True)
        if (ssl["pinSha256"] != sha256(pin_path) or archive.stat().st_size != pin["bytes"]
                or ssl["archiveSha256"] != pin["sha256"] or sha256(archive) != pin["sha256"]
                or source.name != pin["sourceDirectory"]):
            raise AuditSafetyError("Continuation source or pin differs")
        materials = {}
        for row in signature["material"]:
            path = plain_path(Path(row["path"]), root=sdk.root / "home", must_exist=True)
            if path.stat().st_size != row["bytes"] or sha256(path) != row["sha256"]:
                raise AuditSafetyError("Retained official signature/trust material changed")
            materials[path.name] = path
        sdk.audit_inputs = [{"path": str(OWNED / name), "sha256": sha256(OWNED / name)} for name in
                            ("finish_openssl.py", "linux_d_sdk.py", "linux_sdk.py", "dependency_source.py",
                             "audit_support.py", "guest_command.py", "verify_snapshot.py", "git_source.py")]
        sdk.evidence["supplierInputs"] = sdk.audit_inputs
        sdk.environment.update(LANG="C.UTF-8", LC_ALL="C.UTF-8", TZ="UTC")
        flags = ["linux-x86_64", "no-shared", "no-module", "no-tests", "no-docs",
                 "--prefix=" + str(sdk.prefix), "--openssldir=" + str(sdk.prefix / "ssl"),
                 "--libdir=lib", "-fPIC"]
        if any(os.path.lexists(sdk.prefix / name) for name in ("lib/libssl.a", "lib/libcrypto.a", "bin/openssl")):
            raise AuditSafetyError("Existing OpenSSL install is preserved")
        with sdk.writer():
            before = source_check(archive, source)
            if args.resume_build:
                continuation = prior.get("opensslContinuation", {})
                build = plain_path(Path(continuation["newBuildRoot"]), root=sdk.root / "build", must_exist=True)
                configs = ssl.get("buildConfig", [])
                if (before != continuation.get("sourceBefore") or ssl.get("configureArguments") != flags
                        or {Path(row["path"]).name for row in configs} != {"configdata.pm", "Makefile"}):
                    raise AuditSafetyError("Build resume needs the unchanged original source and static recipe")
                for row in configs:
                    path = plain_path(Path(row["path"]), root=build, must_exist=True)
                    if path.parent != build or sha256(path) != row["sha256"]:
                        raise AuditSafetyError("Retained OpenSSL build configuration changed")
                if not any(row.get("argv") == ["make", "-j1", "build_sw"] and row.get("cwd") == str(build)
                           and row.get("state") == "CANCELLED_EXACT_OWNED_STEP" for row in prior["commands"]):
                    raise AuditSafetyError("Only this root's explicitly cancelled static build may resume")
            else:
                build = sdk.root / "build" / ("openssl-" + args.output.stem)
                if os.path.lexists(build):
                    raise AuditSafetyError("Fresh OpenSSL build required")
            sdk.evidence["opensslContinuation"] = {"previousPhase": str(previous),
                "previousPhaseSha256": args.previous_sha256, "previousError": prior.get("error"),
                "previousBuildPreserved": True, "sourceCopied": False, "sourceBefore": before,
                "newBuildRoot": str(build), "sameSourceRecipeBuildResumed": args.resume_build}
            sdk.evidence["openssl"] = {key: value for key, value in ssl.items() if key != "configureArguments"}
            keyring = materials["release-keyring.gpg"]
            detached = materials[pin["signatureFile"]["url"].rsplit("/", 1)[-1]]
            sdk.command(["gpgv", "--homedir", str(keyring.parent), "--keyring", str(keyring),
                         "--status-fd", "1", str(detached), str(archive)], seconds=120)
            log = Path(sdk.evidence["commands"][-1]["log"])
            verified = verify_gpg_status(log.read_bytes())
            verified.update(material=signature["material"], trustAnchor=signature["trustAnchor"],
                            statusLog={"path": str(log), "sha256": sha256(log)})
            sdk.evidence["opensslSignature"] = verified
            sdk.evidence["openssl"]["configureArguments"] = flags
            if not args.resume_build:
                sdk.guard()
                build.mkdir(mode=0o700)
                sdk.command(["perl", str(source / "Configure"), *flags], cwd=build, seconds=900)
                sdk.evidence["openssl"]["buildConfig"] = [{"path": str(build / name), "sha256": sha256(build / name)}
                                                            for name in ("configdata.pm", "Makefile")]
            sdk.save()
            sdk.command(["make", "-j1", "build_sw"], cwd=build, seconds=7200)
            sdk.command(["make", "-j1", "install_sw"], cwd=build, seconds=900)
            sdk.command([str(sdk.prefix / "bin/openssl"), "version", "-a"], seconds=60)
            version_log = Path(sdk.evidence["commands"][-1]["log"])
            if "OpenSSL " + pin["version"] not in version_log.read_text(encoding="utf-8"):
                raise AuditSafetyError("Actual OpenSSL version differs from the official pin")
            sdk.command(["ldd", str(sdk.prefix / "bin/openssl")], seconds=60)
            runtime_log = Path(sdk.evidence["commands"][-1]["log"])
            runtime = runtime_log.read_text(encoding="utf-8")
            if any(value in runtime for value in ("not found", "libssl.so", "libcrypto.so")):
                raise AuditSafetyError("The actual OpenSSL tool uses a missing or shared SSL backend")
            sdk.evidence["openssl"]["staticLibraries"] = [{"path": str(sdk.prefix / "lib" / name),
                "bytes": (sdk.prefix / "lib" / name).stat().st_size, "sha256": sha256(sdk.prefix / "lib" / name)}
                for name in ("libssl.a", "libcrypto.a")]
            after = source_check(archive, source)
            if after != before:
                raise AuditSafetyError("OpenSSL source changed during its build")
            sdk.evidence["opensslContinuation"]["sourceAfter"] = after
            sdk.evidence["openssl"].update(staticBackendRuntimeChecked=True, signatureVerified=True,
                versionLog={"path": str(version_log), "sha256": sha256(version_log)},
                runtimeLog={"path": str(runtime_log), "sha256": sha256(runtime_log)})
            sdk.freeze.postcheck()
            sdk.guard()
            sdk.evidence.update(state="FINISHED", passed=True, finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            sdk.save()
        print(json.dumps({"output": str(sdk.output), "sha256": sha256(sdk.output), "prefix": str(sdk.prefix),
                          "staticOpenSslBuilt": True, "completeProductVerified": False}))
        return 0
    except BaseException as error:
        if sdk is not None:
            sdk.evidence.update(state="FAILED_OR_BLOCKED", passed=False, error=str(error),
                                finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            sdk.save()
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Supply the approved supplemental OpenSSL pin for Linux CI, not Windows TLS."""
from __future__ import annotations

import argparse
import datetime as dt
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile

from ci_evidence import EvidenceError, file_ref, read_json, write_json
from msvc import run_logged


SIGNING_PRIMARY = "B146647E45A7B33947AB226B2A2C87D161692D40"
OFFICIAL_KEYS = "https://www.openssl-library.org/source/pubkeys.asc"
FLAGS = ("linux-x86_64", "no-shared", "no-module", "no-tests", "no-docs", "-fPIC")


def load_pin(path: Path) -> dict:
    pin = read_json(path)
    base = "https://github.com/openssl/openssl/releases/download/openssl-3.5.9/"
    if (pin.get("kind") != "supplemental-nonwindows-dependency-source-pin"
            or pin.get("name") != "openssl" or pin.get("version") != "3.5.9"
            or pin.get("archive") != "openssl-3.5.9.tar.gz" or pin.get("sourceDirectory") != "openssl-3.5.9"
            or pin.get("url") != base + pin["archive"] or pin.get("bytes") != 53279637
            or not re.fullmatch(r"[a-f0-9]{64}", pin.get("sha256", ""))
            or pin.get("metadataAndChecksumMatched") is not True
            or pin.get("signatureFile", {}).get("url") != base + pin["archive"] + ".asc"):
        raise EvidenceError("The approved exact official OpenSSL 3.5.9 pin is required")
    documents = {}
    for key in ("metadataEvidence", "verificationEvidence"):
        row = pin[key]
        relative = PurePosixPath(row["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise EvidenceError("Official pin evidence escapes its source directory")
        evidence = path.parent / relative
        if file_ref(evidence)["sha256"] != row["sha256"]:
            raise EvidenceError("The approved OpenSSL official evidence changed")
        documents[key] = read_json(evidence)
    checked = documents["verificationEvidence"]
    asset = checked.get("archive", {})
    if (checked.get("metadataAndChecksumMatched") is not True or checked.get("checksumSha256") != pin["sha256"]
            or checked.get("version") != pin["version"] or asset.get("name") != pin["archive"]
            or asset.get("digest") != "sha256:" + pin["sha256"] or asset.get("size") != pin["bytes"]
            or asset.get("browser_download_url") != pin["url"]
            or asset not in documents["metadataEvidence"].get("assets", [])):
        raise EvidenceError("OpenSSL source pin is not bound to its official release asset")
    return pin


def verify_signature_status(status: str):
    signatures = []
    for line in status.splitlines():
        words = line.split()
        if len(words) < 2 or words[0] != "[GNUPG:]":
            continue
        if words[1] in {"BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG", "NO_PUBKEY"}:
            raise EvidenceError("OpenSSL release signature is invalid, expired or revoked")
        if words[1] == "VALIDSIG":
            signatures.append(words)
    if (len(signatures) != 1 or len(signatures[0]) < 12
            or signatures[0][-1].upper() != SIGNING_PRIMARY):
        raise EvidenceError("OpenSSL signature does not match the approved official primary key")


def unpack(archive: Path, output: Path, directory: str):
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        if len(members) > 50000 or sum(member.size for member in members) > 512 * 1024 * 1024:
            raise EvidenceError("OpenSSL source expansion exceeds its finite CI budget")
        for member in members:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != directory
                    or member.issym() or member.islnk() or not (member.isfile() or member.isdir())):
                raise EvidenceError("OpenSSL source archive contains an unsafe member")
        bundle.extractall(output, members=members, filter="data")


def supply(options) -> int:
    if sys.platform != "linux" or platform.machine() != "x86_64":
        raise EvidenceError("This exact recipe requires an actual Linux x86_64 host")
    pin = load_pin(options.pin)
    work, prefix = options.work_root.resolve(), options.prefix.resolve()
    if os.path.lexists(work) or os.path.lexists(options.output):
        raise EvidenceError("OpenSSL CI must preserve existing work and evidence")
    parent = work.parent
    while not parent.exists():
        parent = parent.parent
    if shutil.disk_usage(parent).free < 4 * 1024 ** 3:
        raise EvidenceError("OpenSSL CI requires its unchanged 4 GiB dependency reserve")
    work.mkdir(parents=True)
    record = {"schemaVersion": 1, "kind": "native-ci-supplemental-openssl-supply",
              "sourceRef": os.environ.get("PD_SOURCE_REF", ""), "pin": file_ref(options.pin),
              "workRoot": str(work), "prefix": str(prefix), "signingPrimary": SIGNING_PRIMARY,
              "signatureVerified": False, "complete": False, "systemSslUsed": False,
              "productAcceptance": False, "commands": [], "rawInputs": []}
    environment = dict(os.environ)

    def command(argv, cwd=work):
        number = len(record["commands"])
        executable = shutil.which(argv[0])
        if executable is None:
            raise EvidenceError("Required CI supplier tool is unavailable: " + argv[0])
        argv = [executable, *argv[1:]]
        previous = Path.cwd()
        try:
            os.chdir(cwd)
            log, evidence = work / f"command-{number}.log", work / f"command-{number}.json"
            code = run_logged(argv, environment, log, evidence)
        finally:
            os.chdir(previous)
        record["commands"].append({"evidence": file_ref(evidence), "log": file_ref(log)})
        if code:
            raise EvidenceError("OpenSSL CI supplier command failed: " + str(code))
        return log

    def download(url, path, maximum, expected=None):
        command(["curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                 "--connect-timeout", "20", "--max-time", "600", "--max-filesize", str(maximum),
                 "--output", str(path), url])
        actual = file_ref(path)
        if expected and (actual["bytes"] != expected["bytes"] or actual["sha256"] != expected["sha256"]):
            raise EvidenceError("Downloaded official OpenSSL bytes differ from the approved pin")
        record["rawInputs"].append({**actual, "url": url})

    try:
        archive, signature, keys = work / pin["archive"], work / (pin["archive"] + ".asc"), work / "pubkeys.asc"
        download(pin["url"], archive, pin["bytes"], pin)
        download(pin["signatureFile"]["url"], signature, pin["signatureFile"]["bytes"], pin["signatureFile"])
        download(OFFICIAL_KEYS, keys, 1024 * 1024)
        keyring = work / "gnupg"
        keyring.mkdir(mode=0o700)
        gpg = ["gpg", "--homedir", str(keyring), "--batch", "--no-auto-key-retrieve"]
        command([*gpg, "--import", str(keys)])
        status = command([*gpg, "--status-fd", "1", "--verify", str(signature), str(archive)])
        verify_signature_status(status.read_text(encoding="utf-8", errors="replace"))
        record["signatureVerified"] = True
        source_root, build = work / "sources", work / "build"
        source_root.mkdir()
        unpack(archive, source_root, pin["sourceDirectory"])
        source = source_root / pin["sourceDirectory"]
        build.mkdir()
        flags = [*FLAGS, "--prefix=" + str(prefix), "--openssldir=" + str(prefix / "ssl"), "--libdir=lib"]
        command(["perl", str(source / "Configure"), *flags], build)
        command(["make", "-j1", "build_sw"], build)
        command(["make", "-j1", "install_sw"], build)
        command([str(prefix / "bin/openssl"), "version", "-a"])
        record["libraries"] = [file_ref(prefix / "lib" / name) for name in ("libssl.a", "libcrypto.a")]
        record["buildConfiguration"] = [file_ref(build / name) for name in ("Makefile", "configdata.pm")]
        record["complete"] = True
    except (EvidenceError, OSError, subprocess.SubprocessError) as error:
        record["failure"] = str(error)
        return 1
    finally:
        record["finishedUtc"] = dt.datetime.now(dt.UTC).isoformat()
        write_json(options.output, record)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pin", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    try:
        return supply(parser.parse_args())
    except (EvidenceError, OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Supply only owned DrvFS outputs while retaining the existing ext4 Qt SDK."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import uuid

from audit_support import AuditSafetyError, OWNED, atomic_json, owned_output, plain_path, sha256
from dependency_source import SourceFreeze
from linux_sdk import LIMIT, RESERVE_C, RESERVE_D, Sdk
from verify_snapshot import HASH, relative_key


def load_openssl_pin(pin_path: Path) -> dict:
    pin_path = owned_output(pin_path)
    pin = json.loads(pin_path.read_bytes())
    if (not isinstance(pin, dict) or pin.get("kind") != "supplemental-nonwindows-dependency-source-pin"
            or pin.get("name") != "openssl" or pin.get("version") != "3.5.9"
            or not isinstance(pin.get("sha256"), str) or not HASH.fullmatch(pin["sha256"])
            or pin.get("metadataAndChecksumMatched") is not True or pin.get("bytes") != 53279637):
        raise AuditSafetyError("Verified stable supplemental OpenSSL pin required")
    documents = {}
    for name in ("metadataEvidence", "verificationEvidence"):
        record = pin.get(name)
        if not isinstance(record, dict):
            raise AuditSafetyError("OpenSSL pin lacks its official evidence")
        path = plain_path(pin_path.parent / relative_key(record["path"]), root=OWNED, must_exist=True)
        if not path.is_file() or path.stat().st_size > 1024 * 1024 or sha256(path) != record["sha256"]:
            raise AuditSafetyError("OpenSSL official evidence changed")
        documents[name] = json.loads(path.read_bytes())
    verification = documents["verificationEvidence"]
    metadata = documents["metadataEvidence"]
    if (verification.get("metadataAndChecksumMatched") is not True or verification.get("version") != pin["version"]
            or verification.get("checksumSha256") != pin["sha256"]):
        raise AuditSafetyError("OpenSSL pin is not the officially downloaded checksum")
    asset = verification.get("archive", {})
    if (asset.get("name") != pin["archive"] or asset.get("size") != pin["bytes"]
            or asset.get("digest") != "sha256:" + pin["sha256"] or asset.get("browser_download_url") != pin["url"]
            or asset not in metadata.get("assets", [])):
        raise AuditSafetyError("OpenSSL pin differs from the official release asset")
    api = pin.get("officialReleaseApi")
    command = next((row for row in metadata.get("commands", []) if row.get("argv") ==
                    ["gh", "api", "repos/openssl/openssl/releases/tags/openssl-3.5.9"] and row.get("exitCode") == 0), None)
    if api != "https://api.github.com/repos/openssl/openssl/releases/tags/openssl-3.5.9" or command is None:
        raise AuditSafetyError("OpenSSL release API origin is not the official pinned tag")
    release = json.loads(command["stdout"])
    if release.get("tag_name") != "openssl-3.5.9" or release.get("draft") or release.get("prerelease"):
        raise AuditSafetyError("OpenSSL release is not the stable official tag")
    if not pin["url"].startswith("https://github.com/openssl/openssl/releases/download/openssl-3.5.9/"):
        raise AuditSafetyError("OpenSSL source origin is not official")
    return pin


OPENSSL_SIGNING_FINGERPRINT = "B146647E45A7B33947AB226B2A2C87D161692D40"


def verify_gpg_status(text: str | bytes) -> dict:
    # GPG NOTATION_DATA can contain arbitrary octets; do not decode the combined log as UTF-8.
    lines = [line.decode("latin-1") for line in text.split(b"\n")] if isinstance(text, bytes) else text.splitlines()
    statuses = [line.split()[1:] for line in lines if line.startswith("[GNUPG:] ")]
    invalid = {"BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG", "KEYEXPIRED", "SIGEXPIRED", "FAILURE", "ERROR"}
    if any(row and row[0] in invalid for row in statuses):
        raise AuditSafetyError("OpenSSL detached signature has a failing GPG status")
    valid = [row for row in statuses if row and row[0] == "VALIDSIG"]
    if (len(valid) != 1 or len(valid[0]) != 11 or valid[0][-1] != OPENSSL_SIGNING_FINGERPRINT
            or valid[0][1] == "" or not any(row and row[0] == "GOODSIG" for row in statuses)):
        raise AuditSafetyError("OpenSSL signature does not chain to the official primary fingerprint")
    return {"primaryFingerprint": valid[0][-1], "signingFingerprint": valid[0][1],
            "signatureDate": valid[0][2], "signatureTimestamp": valid[0][3], "cryptographicSignatureVerified": True}


class DriveSdk(Sdk):
    def __init__(self, root: Path, token: str, output: Path, qt_owned: Path, qt_supply: Path,
                 qt_supply_sha256: str, manifest: Path, manifest_sha256: str, create: bool = False):
        if sys.platform != "linux" or not hasattr(os, "pidfd_open"):
            raise AuditSafetyError("Actual Linux pidfd and DrvFS host required")
        self.output = owned_output(output)
        if self.output.exists():
            raise AuditSafetyError("Existing supply evidence is preserved")
        self.root = plain_path(root, root=OWNED / "scratch")
        if not self.root.is_relative_to(Path("/mnt/d")):
            raise AuditSafetyError("New dependency/build output must be on /mnt/d")
        self.freeze = SourceFreeze(manifest, manifest_sha256)
        self.qt_owned = owned_output(qt_owned)
        self.qt_supply = owned_output(qt_supply)
        if not HASH.fullmatch(qt_supply_sha256) or sha256(self.qt_supply) != qt_supply_sha256:
            raise AuditSafetyError("Qt supply evidence differs from its explicit pin")
        ownership = json.loads(self.qt_owned.read_bytes())["marker"]
        supplier = json.loads(self.qt_supply.read_bytes())
        if supplier.get("state") != "FINISHED" or supplier.get("passed") is not True:
            raise AuditSafetyError("The exact retained Qt supplier did not finish")
        self.qt_root = plain_path(Path(ownership["root"]), must_exist=True)
        self.qt_marker = plain_path(self.qt_root / ".nickel-portability-owned.json", must_exist=True)
        self.qt_marker_bytes = self.qt_marker.read_bytes()
        info = self.qt_root.stat()
        self.qt_identity = (info.st_dev, info.st_ino, info.st_uid)
        if (ownership != json.loads(self.qt_marker_bytes) or ownership.get("owner") != str(OWNED)
                or ownership.get("kind") != "portability-owned-linux-sdk"
                or self.qt_identity != (ownership["device"], ownership["inode"], ownership["uid"])
                or supplier.get("markerSha256") != hashlib.sha256(self.qt_marker_bytes).hexdigest()):
            raise AuditSafetyError("Retained ext4 Qt root ownership differs")
        self.qt_prefix = plain_path(Path(supplier["qt"]["prefix"]), root=self.qt_root, must_exist=True)
        self.qt_configs = supplier["qt"]["configs"]
        for row in self.qt_configs:
            path = plain_path(Path(row["path"]), root=self.qt_prefix, must_exist=True)
            if sha256(path) != row["sha256"]:
                raise AuditSafetyError("Retained Qt configuration differs")
        self.qt_supply_sha256 = qt_supply_sha256
        self.evidence = {"kind": "actual-linux-d-sdk-and-product-build", "state": "STARTING",
                         "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(), "workerPid": os.getpid(),
                         "root": str(self.root), "commands": [], "sources": [], "passed": False,
                         "completeProductVerified": False, "dependencySource": self.freeze.evidence,
                         "retainedQt": {"root": str(self.qt_root), "prefix": str(self.qt_prefix),
                                        "ownershipEvidence": str(self.qt_owned), "ownershipEvidenceSha256": sha256(self.qt_owned),
                                        "supplyEvidence": str(self.qt_supply), "supplyEvidenceSha256": qt_supply_sha256,
                                        "readOnly": True, "duplicated": False},
                         "scope": "Only marked D dependency/prefix/product outputs, existing Qt retained; a build is not GUI/deployment acceptance"}
        if create:
            if os.path.lexists(self.root):
                raise AuditSafetyError("Existing D SDK root is preserved")
            self.host_preflight(LIMIT)
            self.root.mkdir(mode=0o700)
            info = self.root.stat()
            marker = {"kind": "portability-owned-linux-d-sdk", "owner": str(OWNED), "root": str(self.root),
                      "token": uuid.uuid4().hex, "uid": os.getuid(), "device": info.st_dev, "inode": info.st_ino,
                      "limitBytes": LIMIT, "hostBackingDrive": "D", "distribution": "Ubuntu-24.04",
                      "retainedQtRoot": str(self.qt_root), "retainedQtMarkerSha256": hashlib.sha256(self.qt_marker_bytes).hexdigest(),
                      "sourceManifestSha256": manifest_sha256, "sourceSetSha256": self.freeze.evidence["sourceSetSha256"]}
            (self.root / ".nickel-portability-owned.json").write_text(json.dumps(marker, indent=2) + "\n", encoding="ascii")
            token = marker["token"]
        else:
            marker = json.loads(plain_path(self.root / ".nickel-portability-owned.json", must_exist=True).read_bytes())
        info = self.root.stat()
        if (marker.get("kind") != "portability-owned-linux-d-sdk" or marker.get("owner") != str(OWNED)
                or marker.get("root") != str(self.root) or marker.get("token") != token
                or marker.get("uid") != os.getuid() or marker.get("device") != info.st_dev
                or marker.get("inode") != info.st_ino or marker.get("limitBytes") != LIMIT
                or marker.get("retainedQtRoot") != str(self.qt_root)
                or marker.get("sourceManifestSha256") != manifest_sha256):
            raise AuditSafetyError("D SDK marker does not bind this root/token/source")
        self.token = token
        self.control = self.output.with_suffix(".control.json")
        self.marker_bytes = (self.root / ".nickel-portability-owned.json").read_bytes()
        self.root_identity = (info.st_dev, info.st_ino, info.st_uid)
        self.prefix = self.root / "prefix"
        self.openssl_root = self.prefix
        self.environment = dict(os.environ)
        self.environment.update(TEMP=str(self.root / "tmp"), TMP=str(self.root / "tmp"), TMPDIR=str(self.root / "tmp"),
                                HOME=str(self.root / "home"), XDG_CACHE_HOME=str(self.root / "home/cache"),
                                XDG_CONFIG_HOME=str(self.root / "home/config"), XDG_DATA_HOME=str(self.root / "home/data"),
                                PYTHONDONTWRITEBYTECODE="1", CMAKE_BUILD_PARALLEL_LEVEL="1", CCACHE_DISABLE="1",
                                LD_LIBRARY_PATH=str(self.qt_prefix / "lib") + ":" + str(self.root / "runtime/usr/lib/x86_64-linux-gnu"),
                                PKG_CONFIG_PATH=str(self.prefix / "lib/pkgconfig"),
                                SSL_CERT_FILE="/etc/ssl/certs/ca-certificates.crt", SSL_CERT_DIR="/etc/ssl/certs")
        self.guard()
        for name in ("tmp", "home", "build", "sources", "prefix", "downloads"):
            plain_path(self.root / name, root=self.root).mkdir(exist_ok=True, mode=0o700)
        self.guard()
        self.evidence.update(token=token, marker=marker, markerSha256=hashlib.sha256(self.marker_bytes).hexdigest())
        self.save()

    @staticmethod
    def host_preflight(required: int = 0) -> dict:
        free_c, free_d = shutil.disk_usage("/mnt/c").free, shutil.disk_usage("/mnt/d").free
        result = {"freeCBytes": free_c, "freeDBytes": free_d, "reserveCBytes": RESERVE_C,
                  "reserveDBytes": RESERVE_D, "requiredDBytes": required}
        if free_c < RESERVE_C or free_d < RESERVE_D + required:
            raise AuditSafetyError("Physical host reserve before D write: " + json.dumps(result))
        return result

    @staticmethod
    def measure(root: Path) -> dict:
        logical = allocated = count = vanished = 0
        for directory, folders, files in os.walk(root, followlinks=False):
            for name in folders + files:
                path = Path(directory) / name
                count += 1
                if count > 120000:
                    raise AuditSafetyError("SDK inventory exceeded bounded member count")
                try:
                    info = path.lstat()
                except FileNotFoundError:
                    if name not in files or not path.is_relative_to(root / "build") or not name.endswith(".d.tmp"):
                        raise
                    # GCC renames this mutable dependency file; immutable missing members remain fatal.
                    allocated += 4096
                    vanished += 1
                    continue
                allocated += info.st_blocks * 512
                if not stat.S_ISDIR(info.st_mode):
                    logical += info.st_size
        return {"logicalBytes": logical, "allocatedBytes": allocated, "chargedBytes": max(logical, allocated),
                "entries": count, "vanishedBuildEntries": vanished}

    def guard(self) -> dict:
        self.freeze.check()
        product_source = getattr(self, "product_freeze", None)
        if product_source is not None:
            product_source.check()
        for root, identity, marker_bytes in ((self.root, self.root_identity, self.marker_bytes),
                                             (self.qt_root, self.qt_identity, self.qt_marker_bytes)):
            root = plain_path(root, must_exist=True)
            info = root.stat()
            if (info.st_dev, info.st_ino, info.st_uid) != identity:
                raise AuditSafetyError("SDK root identity changed")
            if plain_path(root / ".nickel-portability-owned.json", must_exist=True).read_bytes() != marker_bytes:
                raise AuditSafetyError("SDK root marker changed")
        owned, qt = self.measure(self.root), self.measure(self.qt_root)
        free_c, free_d = shutil.disk_usage("/mnt/c").free, shutil.disk_usage("/mnt/d").free
        charged = owned["chargedBytes"] + qt["chargedBytes"]
        result = {"freeCBytes": free_c, "freeDBytes": free_d, "reserveCBytes": RESERVE_C,
                  "reserveDBytes": RESERVE_D, "chargedBytes": charged, "limitBytes": LIMIT,
                  "ownedD": owned, "retainedQt": qt}
        self.evidence["latestCapacity"] = result
        if free_c < RESERVE_C or free_d < RESERVE_D or charged > LIMIT:
            raise AuditSafetyError("Aggregate SDK/physical host reserve: " + json.dumps(result))
        if sha256(self.qt_supply) != self.qt_supply_sha256:
            raise AuditSafetyError("Retained Qt evidence changed")
        for row in getattr(self, "audit_inputs", []):
            if sha256(Path(row["path"])) != row["sha256"]:
                raise AuditSafetyError("Supplier source changed during the actual build")
        receipt = getattr(self, "openssl_supply", None)
        if receipt is not None and sha256(receipt["path"]) != receipt["sha256"]:
            raise AuditSafetyError("Pinned OpenSSL supply receipt changed")
        return result

    @contextmanager
    def writer(self):
        self.guard()
        lock = plain_path(self.qt_root / ".sdk-writer.lock", root=self.qt_root, must_exist=True)
        fd = os.open(lock, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with super().writer():
                yield
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    def qt(self, *args, **kwargs):
        raise AuditSafetyError("D split supply never reinstalls or duplicates retained ext4 Qt")

    def runtime_inventory(self, runtime: Path) -> list[dict]:
        rows = []
        for directory, folders, files in os.walk(runtime, followlinks=False):
            for name in sorted(folders + files):
                path = Path(directory) / name
                info = path.lstat()
                row = {"path": path.relative_to(runtime).as_posix(), "mode": stat.S_IFMT(info.st_mode)}
                if stat.S_ISLNK(info.st_mode):
                    row["link"] = os.readlink(path)
                elif stat.S_ISREG(info.st_mode):
                    if info.st_size > 50 * 1024 * 1024:
                        raise AuditSafetyError("Oversized platform runtime member")
                    row.update(bytes=info.st_size, sha256=sha256(path))
                elif not stat.S_ISDIR(info.st_mode):
                    raise AuditSafetyError("Unexpected special platform runtime member")
                rows.append(row)
                if len(rows) > 5000:
                    raise AuditSafetyError("Platform runtime member count exceeded")
        return sorted(rows, key=lambda row: row["path"])

    def platform_runtime(self, index_path: Path, previous: Path | None = None,
                         previous_sha256: str | None = None):
        self.guard()
        index_path = owned_output(index_path)
        envelope = json.loads(index_path.read_bytes())
        index = json.loads(envelope["command"]["stdout"])
        names = {"libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1"}
        packages = index.get("packages", [])
        if (envelope["command"].get("passed") is not True or index.get("exitCode") != 0
                or len(packages) != 8 or {row.get("Package") for row in packages} != names):
            raise AuditSafetyError("The exact eight-package signed-index runtime discovery is required")
        total = 0
        for row in packages:
            if (row.get("Architecture") != "amd64" or not isinstance(row.get("SHA256"), str)
                    or not HASH.fullmatch(row["SHA256"]) or int(row["Size"]) <= 0
                    or not isinstance(row.get("Version"), str) or not row["Version"]):
                raise AuditSafetyError("Invalid pinned platform package")
            relative_key(row["Filename"])
            total += int(row["Size"])
        if total > 50 * 1024 * 1024:
            raise AuditSafetyError("Platform runtime archives exceed 50 MiB")
        runtime = plain_path(self.root / "runtime", root=self.root)
        downloads = plain_path(self.root / "runtime-downloads", root=self.root)
        completed = []
        if runtime.exists() or downloads.exists():
            if previous is None or not isinstance(previous_sha256, str) or not HASH.fullmatch(previous_sha256):
                raise AuditSafetyError("Existing platform supply is preserved; explicit previous phase pin required")
            previous = owned_output(previous)
            if not previous.is_file() or previous.stat().st_size > 1024 * 1024 or sha256(previous) != previous_sha256:
                raise AuditSafetyError("Previous platform phase differs from its explicit pin")
            receipt = json.loads(previous.read_bytes())
            prior = receipt.get("platformRuntime", {})
            if (receipt.get("kind") != "actual-linux-d-sdk-and-product-build"
                    or receipt.get("state") != "FAILED_OR_BLOCKED" or receipt.get("token") != self.token
                    or receipt.get("root") != str(self.root)
                    or receipt.get("markerSha256") != hashlib.sha256(self.marker_bytes).hexdigest()
                    or receipt.get("dependencySource", {}).get("manifestSha256") != self.freeze.expected_sha256
                    or prior.get("indexSha256") != sha256(index_path) or prior.get("prefix") != str(runtime)
                    or not runtime.is_dir() or not downloads.is_dir()):
                raise AuditSafetyError("Previous platform phase does not bind this owner/root/index/source")
            inventory = self.runtime_inventory(runtime)
            if inventory != prior.get("runtimeInventory", []):
                raise AuditSafetyError("Partial runtime changed or lacks an extraction receipt")
            completed = prior.get("packages", [])
            if not isinstance(completed, list):
                raise AuditSafetyError("Previous package receipts must be a list")
            by_name = {row["Package"]: row for row in packages}
            for row in completed:
                if not isinstance(row, dict) or row.get("Package") not in by_name:
                    raise AuditSafetyError("Unexpected prior runtime package")
                if any(row.get(key) != value for key, value in by_name[row["Package"]].items()):
                    raise AuditSafetyError("Prior package differs from the exact index")
            expected_archives = {Path(row["Filename"]).name: row for row in packages}
            for path in downloads.iterdir():
                row = expected_archives.get(path.name)
                path = plain_path(path, root=downloads, must_exist=True)
                if (row is None or not path.is_file() or path.stat().st_size != int(row["Size"])
                        or sha256(path) != row["SHA256"]):
                    raise AuditSafetyError("Existing download differs from the exact runtime index")
            self.evidence["runtimeResume"] = {"previousPath": str(previous), "previousSha256": previous_sha256,
                                              "sameRoot": True, "existingArchivesPreserved": True}
        else:
            if previous is not None:
                raise AuditSafetyError("Platform resume requires the original partial directories")
            self.guard()
            runtime.mkdir(mode=0o700)
            downloads.mkdir(mode=0o700)
        self.evidence["platformRuntime"] = {"indexPath": str(index_path), "indexSha256": sha256(index_path),
                                            "prefix": str(runtime), "systemPackagesUnchanged": True,
                                            "packages": list(completed), "runtimeInventory": self.runtime_inventory(runtime)}
        done = {row["Package"] for row in completed}
        for row in packages:
            archive = plain_path(downloads / Path(row["Filename"]).name, root=self.root)
            if not archive.exists():
                self.command(["apt-get", "download", row["Package"] + "=" + row["Version"]], cwd=downloads, seconds=120)
            if archive.stat().st_size != int(row["Size"]) or sha256(archive) != row["SHA256"]:
                raise AuditSafetyError("Runtime download differs from signed apt index")
            if row["Package"] not in done:
                self.command(["dpkg-deb", "--extract", str(archive), str(runtime)], seconds=30)
                self.evidence["platformRuntime"]["packages"].append({**row, "archivePath": str(archive)})
                self.evidence["platformRuntime"]["runtimeInventory"] = self.runtime_inventory(runtime)
                self.save()
            if self.measure(runtime)["chargedBytes"] + self.measure(downloads)["chargedBytes"] > 50 * 1024 * 1024:
                raise AuditSafetyError("Platform runtime archive plus unpacked supply exceeded 50 MiB")
        self.command(["ldd", str(self.qt_prefix / "plugins/platforms/libqxcb.so")], seconds=30)
        output = Path(self.evidence["commands"][-1]["log"]).read_text(encoding="utf-8")
        if "not found" in output:
            raise AuditSafetyError("Retained Qt xcb plugin still has missing libraries")
        self.evidence["platformRuntime"]["lddClosureChecked"] = True
        self.save()

    def openssl_signature(self, pin: dict, pin_path: Path, archive: Path, *, verification_home: Path | None = None) -> dict:
        self.guard()
        home = plain_path(verification_home or self.root / "home/openssl-gpg", root=self.root / "home")
        if home.exists():
            raise AuditSafetyError("Existing OpenSSL GPG verification material is preserved")
        home.mkdir(mode=0o700)
        evidence_path = plain_path(pin_path.parent / relative_key(pin["verificationEvidence"]["path"]),
                                   root=OWNED, must_exist=True)
        verified = json.loads(evidence_path.read_bytes())
        signature_row = next((row for row in verified.get("commands", []) if row.get("exitCode") == 0
                              and row.get("argv", [])[-1:] == [pin["signatureFile"]["url"]]), None)
        if signature_row is None:
            raise AuditSafetyError("The pinned official detached signature was not supplied")
        signature_bytes = signature_row["stdout"].encode("utf-8")
        if (len(signature_bytes) != pin["signatureFile"]["bytes"]
                or hashlib.sha256(signature_bytes).hexdigest() != pin["signatureFile"]["sha256"]):
            raise AuditSafetyError("Retained official detached signature differs from the pin")
        signature = home / pin["signatureFile"]["url"].rsplit("/", 1)[-1]
        signature.write_bytes(signature_bytes)
        keys, page, keyring = home / "pubkeys.asc", home / "official-source.html", home / "release-keyring.gpg"
        for destination, url, size in ((keys, "https://openssl-library.org/source/pubkeys.asc", 1024 * 1024),
                                       (page, "https://openssl-library.org/source/", 256 * 1024)):
            self.command(["curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                          "--connect-timeout", "15", "--max-time", "60", "--max-filesize", str(size),
                          "--output", str(destination), url], seconds=75)
        from html.parser import HTMLParser

        class Text(HTMLParser):
            def __init__(self):
                super().__init__()
                self.parts = []

            def handle_data(self, data):
                self.parts.append(data)

        parsed = Text()
        parsed.feed(page.read_text(encoding="utf-8"))
        if OPENSSL_SIGNING_FINGERPRINT not in "".join("".join(parsed.parts).split()).upper():
            raise AuditSafetyError("Official OpenSSL source page lacks the canonical primary fingerprint")
        self.command(["gpg", "--no-options", "--batch", "--no-autostart", "--homedir", str(home),
                      "--with-colons", "--show-keys", str(keys)], seconds=30)
        keys_log = Path(self.evidence["commands"][-1]["log"])
        if b":" + OPENSSL_SIGNING_FINGERPRINT.encode("ascii") + b":" not in keys_log.read_bytes():
            raise AuditSafetyError("Official release key bundle lacks the pinned primary fingerprint")
        self.command(["gpg", "--no-options", "--batch", "--no-autostart", "--homedir", str(home),
                      "--output", str(keyring), "--dearmor", str(keys)], seconds=30)
        self.command(["gpgv", "--homedir", str(home), "--keyring", str(keyring), "--status-fd", "1",
                      str(signature), str(archive)], seconds=60)
        status_log = Path(self.evidence["commands"][-1]["log"])
        result = verify_gpg_status(status_log.read_bytes())
        result.update(trustAnchor="Official HTTPS source page canonical fingerprint; no user keyring or keyserver",
                      statusLog={"path": str(status_log), "sha256": sha256(status_log)},
                      material=[{"path": str(path), "bytes": path.stat().st_size, "sha256": sha256(path)}
                                for path in (keys, page, signature, keyring)], completeProductVerified=False)
        self.evidence["opensslSignature"] = result
        self.save()
        return result

    def openssl(self, pin_path: Path, previous: Path | None = None, previous_sha256: str | None = None):
        pin_path = owned_output(pin_path)
        raw = pin_path.read_bytes()
        pin = load_openssl_pin(pin_path)
        self.guard()
        archive = plain_path(self.root / "downloads" / relative_key(pin["archive"]), root=self.root)
        source = plain_path(self.root / "sources" / relative_key(pin["sourceDirectory"]), root=self.root)
        build = plain_path(self.root / "build/openssl", root=self.root)
        if os.path.lexists(source) or os.path.lexists(build):
            raise AuditSafetyError("Existing OpenSSL source/build is preserved; archive-only resume cannot reidentify it")
        if any(os.path.lexists(self.prefix / name) for name in ("lib/libssl.a", "lib/libcrypto.a", "bin/openssl")):
            raise AuditSafetyError("An existing OpenSSL install is preserved")
        if (previous is None) != (previous_sha256 is None):
            raise AuditSafetyError("OpenSSL archive resume needs the previous phase and its explicit SHA-256")
        if os.path.lexists(archive):
            if previous is None or not isinstance(previous_sha256, str) or not HASH.fullmatch(previous_sha256):
                raise AuditSafetyError("Existing OpenSSL archive requires explicit same-root resume")
            previous = owned_output(previous)
            if previous.stat().st_size > 1024 * 1024 or sha256(previous) != previous_sha256:
                raise AuditSafetyError("Previous OpenSSL phase differs from its explicit pin")
            prior = json.loads(previous.read_bytes())
            retained = prior.get("openssl", {})
            if (prior.get("kind") != self.evidence["kind"] or prior.get("state") != "FAILED_OR_BLOCKED"
                    or prior.get("passed") is not False or prior.get("root") != str(self.root)
                    or prior.get("token") != self.token
                    or prior.get("markerSha256") != hashlib.sha256(self.marker_bytes).hexdigest()
                    or prior.get("dependencySource", {}).get("manifestSha256") != self.freeze.expected_sha256
                    or retained.get("pinSha256") != hashlib.sha256(raw).hexdigest()
                    or retained.get("archivePath") != str(archive) or retained.get("archiveSha256") != pin["sha256"]
                    or retained.get("archiveBytes") != pin["bytes"] or retained.get("systemSslUsed") is not False
                    or not archive.is_file() or archive.stat().st_size != pin["bytes"] or sha256(archive) != pin["sha256"]):
                raise AuditSafetyError("Previous phase does not bind this root's unchanged pinned OpenSSL archive")
            self.evidence["opensslResume"] = {"previousPhase": str(previous), "previousPhaseSha256": previous_sha256,
                                               "previousState": prior["state"], "previousError": prior.get("error"),
                                               "archiveReused": True, "previousMaterialPreserved": True}
        elif previous is not None:
            raise AuditSafetyError("OpenSSL resume cannot invent a missing retained archive")
        else:
            self.command(["curl", "--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                          "--connect-timeout", "20", "--max-time", "600", "--max-filesize", str(pin["bytes"]),
                          "--output", str(archive), pin["url"]], seconds=630)
        if archive.stat().st_size != pin["bytes"] or sha256(archive) != pin["sha256"]:
            raise AuditSafetyError("Downloaded OpenSSL source differs from the official pin")
        self.evidence["openssl"] = {"pinPath": str(pin_path), "pinSha256": hashlib.sha256(raw).hexdigest(),
                                    "archivePath": str(archive), "archiveSha256": pin["sha256"],
                                    "archiveBytes": pin["bytes"], "signatureVerified": False, "systemSslUsed": False}
        self.save()
        verification_home = self.root / ("home/openssl-gpg-" + self.output.stem) if previous is not None else None
        signature = self.openssl_signature(pin, pin_path, archive, verification_home=verification_home)
        self.evidence["openssl"]["signatureVerified"] = signature["cryptographicSignatureVerified"]
        with tarfile.open(archive) as bundle:
            members = bundle.getmembers()
            expanded = sum(row.size for row in members)
            if self.guard()["chargedBytes"] + expanded + len(members) * 4096 > LIMIT:
                raise AuditSafetyError("OpenSSL expansion exceeds aggregate quota")
            for row in members:
                if row.name.startswith("/") or ".." in Path(row.name).parts:
                    raise AuditSafetyError("OpenSSL archive escapes owned root")
            bundle.extractall(self.root / "sources", members=members, filter="data")
        self.evidence["openssl"].update(sourcePath=str(source), expandedBytes=expanded)
        self.save()
        self.guard()
        build.mkdir(mode=0o700)
        flags = ["linux-x86_64", "no-shared", "no-module", "no-tests", "no-docs",
                 "--prefix=" + str(self.prefix), "--openssldir=" + str(self.prefix / "ssl"), "--libdir=lib", "-fPIC"]
        self.evidence["openssl"]["configureArguments"] = flags
        self.command(["perl", str(source / "Configure"), *flags], cwd=build, seconds=180)
        self.command(["make", "-j1", "build_sw"], cwd=build, seconds=1800)
        self.command(["make", "-j1", "install_sw"], cwd=build, seconds=180)
        self.command([str(self.prefix / "bin/openssl"), "version", "-a"], seconds=30)
        self.evidence["openssl"]["buildConfig"] = [{"path": str(build / name), "sha256": sha256(build / name)}
                                                    for name in ("configdata.pm", "Makefile")]
        self.evidence["openssl"]["staticLibraries"] = [{"path": str(self.prefix / "lib" / name),
                                                         "bytes": (self.prefix / "lib" / name).stat().st_size,
                                                         "sha256": sha256(self.prefix / "lib" / name)}
                                                        for name in ("libssl.a", "libcrypto.a")]
        self.freeze.postcheck()
        self.save()

    def bind_git_source(self, manifest: Path, digest: str, commit: str):
        from git_source import GitSourceFreeze
        self.guard()
        source = GitSourceFreeze(manifest, digest, commit)
        if not source.root.is_relative_to(self.root / "sources"):
            raise AuditSafetyError("New product source must be inside this SDK's charged owned sources")
        self.product_freeze = source
        self.evidence["productSource"] = source.evidence
        old = {row["path"]: row["sha256"] for row in self.freeze.inputs}
        current = {row["path"]: row["sha256"] for row in source.inputs}
        self.evidence["recipeComparison"] = [{"path": key, "historicalSha256": old[key],
                                               "productSha256": current[key], "identical": old[key] == current[key]}
                                              for key in old if key in current]
        self.evidence["historicalMarkerPreserved"] = True
        self.save()

    def bind_openssl_supply(self, path: Path, digest: str):
        self.guard()
        path = owned_output(path)
        if not isinstance(digest, str) or not HASH.fullmatch(digest) or sha256(path) != digest:
            raise AuditSafetyError("Explicit unchanged OpenSSL supply receipt required")
        receipt = json.loads(path.read_bytes())
        ssl = receipt.get("openssl", {})
        pin_path = OWNED / "openssl-linux-pin.json"
        pin = load_openssl_pin(pin_path)
        if (receipt.get("state") != "FINISHED" or receipt.get("passed") is not True
                or receipt.get("root") != str(self.root) or receipt.get("token") != self.token
                or receipt.get("markerSha256") != hashlib.sha256(self.marker_bytes).hexdigest()
                or ssl.get("pinSha256") != sha256(pin_path) or ssl.get("archiveSha256") != pin["sha256"]
                or ssl.get("archiveBytes") != pin["bytes"] or ssl.get("signatureVerified") is not True
                or ssl.get("systemSslUsed") is not False
                or receipt.get("opensslSignature", {}).get("primaryFingerprint") != OPENSSL_SIGNING_FINGERPRINT
                or receipt.get("opensslSignature", {}).get("cryptographicSignatureVerified") is not True):
            raise AuditSafetyError("OpenSSL receipt lacks this root's pinned verified static supply")
        libraries = ssl.get("staticLibraries", [])
        if not isinstance(libraries, list) or {Path(row["path"]).name for row in libraries} != {"libssl.a", "libcrypto.a"}:
            raise AuditSafetyError("The actual static OpenSSL library pair is required")
        for row in libraries:
            library = plain_path(Path(row["path"]), root=self.prefix / "lib", must_exist=True)
            if not library.is_file() or library.stat().st_size != row["bytes"] or sha256(library) != row["sha256"]:
                raise AuditSafetyError("Installed static OpenSSL library differs from its supply receipt")
        self.openssl_supply = {"path": path, "sha256": digest, "libraries": libraries}
        self.evidence["opensslSupply"] = {"path": str(path), "sha256": digest, "libraries": libraries,
                                          "sameBackend": True, "systemSslUsed": False}
        self.save()

    def product(self):
        self.guard()
        source = getattr(self, "product_freeze", None)
        if source is None or getattr(self, "openssl_supply", None) is None:
            raise AuditSafetyError("Product needs an explicit fresh Git source and verified pinned OpenSSL supply")
        metadata = source.evidence["metadata"]
        if metadata.get("displayName") != "Palladium" or metadata.get("executableName") != "pd-launcher":
            raise AuditSafetyError("New product source must carry the actual Palladium metadata")
        build = self.root / "build" / ("product-" + source.commit[:12])
        if os.path.lexists(build):
            raise AuditSafetyError("Product build must be fresh; old objects are not reidentified")
        target = source.evidence["productTarget"]
        self.evidence["productBuild"] = {"sourceRoot": str(source.root), "manifestSha256": source.expected_sha256,
                                         "gitCommit": source.commit, "sourceSetSha256": source.evidence["sourceSetSha256"],
                                         "buildRoot": str(build), "configuration": "Release", "target": target,
                                         "executableName": metadata["executableName"], "compositeSourceUsed": False,
                                         "completeProductVerified": False, "guiRuntimeVerified": False,
                                         "productCTestRun": False, "productCTestPassed": False}
        self.save()
        self.command(["cmake", "--debug-trycompile", "-S", str(source.root), "-B", str(build), "-G", "Ninja",
                      "-DCMAKE_BUILD_TYPE=Release", "-DNICKEL_BUILD_UI=ON", "-DNICKEL_BUILD_TESTS=ON",
                      "-DNICKEL_SHARED_NATIVE_ROOT=", "-DCMAKE_PREFIX_PATH=" + str(self.prefix) + ";" + str(self.qt_prefix),
                      "-DCMAKE_INSTALL_PREFIX=" + str(self.root / "deploy" / source.commit[:12]),
                      "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                      "-DOPENSSL_USE_STATIC_LIBS=ON", "-DOPENSSL_ROOT_DIR=" + str(self.prefix),
                      "-DOPENSSL_INCLUDE_DIR=" + str(self.prefix / "include"),
                      "-DOPENSSL_SSL_LIBRARY=" + str(self.prefix / "lib/libssl.a"),
                      "-DOPENSSL_CRYPTO_LIBRARY=" + str(self.prefix / "lib/libcrypto.a"),
                      "-DZLIB_LIBRARY=" + str(self.prefix / "lib/libz.a"), "-DZLIB_INCLUDE_DIR=" + str(self.prefix / "include")], seconds=900)
        source.postcheck()
        self.evidence["productBuild"].update(cacheSha256=sha256(build / "CMakeCache.txt"), configuredBeforeObjects=True)
        self.save()
        self.command(["cmake", "--build", str(build), "--parallel", "1"], seconds=7200)
        source.postcheck()
        self.guard()
        binaries = [path for path in build.rglob(metadata["executableName"]) if path.is_file() and not path.is_symlink()]
        if len(binaries) != 1:
            raise AuditSafetyError("Exactly one actual complete launcher executable must be built")
        binary = plain_path(binaries[0], root=build, must_exist=True)
        self.evidence["productBuild"].update(buildFinished=True, artifact={"path": str(binary),
                                            "bytes": binary.stat().st_size, "sha256": sha256(binary)})
        self.save()
        self.evidence["productBuild"]["productCTestRun"] = True
        self.save()
        self.command(["ctest", "--test-dir", str(build), "--output-on-failure", "--parallel", "1", "--timeout", "120"],
                     seconds=1800)
        source.postcheck()
        self.evidence["productBuild"]["productCTestPassed"] = True
        self.save()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--token", default="")
    parser.add_argument("--create-root", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-owned-evidence", type=Path, required=True)
    parser.add_argument("--qt-supply-evidence", type=Path, required=True)
    parser.add_argument("--qt-supply-sha256", required=True)
    parser.add_argument("--snapshot-manifest", type=Path, required=True)
    parser.add_argument("--expected-manifest-sha256", required=True)
    parser.add_argument("--only", choices=("create", "platform", "openssl", "deps", "product"), required=True)
    parser.add_argument("--platform-index", type=Path)
    parser.add_argument("--previous-platform", type=Path)
    parser.add_argument("--previous-platform-sha256")
    parser.add_argument("--openssl-pin", type=Path)
    parser.add_argument("--previous-openssl", type=Path)
    parser.add_argument("--previous-openssl-sha256")
    parser.add_argument("--openssl-supply", type=Path)
    parser.add_argument("--openssl-supply-sha256")
    parser.add_argument("--product-source-manifest", type=Path)
    parser.add_argument("--product-source-sha256")
    parser.add_argument("--product-git-commit")
    parser.add_argument("--dependencies", nargs="+")
    parser.add_argument("--archive-root", type=Path, help="Plain physical directory containing read-only pinned archives")
    args = parser.parse_args()
    if (args.previous_platform is None) != (args.previous_platform_sha256 is None):
        parser.error("Runtime resume requires both previous phase path and SHA-256")
    if (args.previous_openssl is None) != (args.previous_openssl_sha256 is None):
        parser.error("OpenSSL archive resume requires both previous phase path and SHA-256")
    if (args.openssl_supply is None) != (args.openssl_supply_sha256 is None):
        parser.error("OpenSSL supply needs both receipt path and SHA-256")
    source_options = (args.product_source_manifest, args.product_source_sha256, args.product_git_commit)
    if any(value is not None for value in source_options) and not all(value is not None for value in source_options):
        parser.error("Product source needs its manifest path, SHA-256 and full Git commit")
    sdk = None
    try:
        sdk = DriveSdk(args.root, args.token, args.output, args.qt_owned_evidence, args.qt_supply_evidence,
                       args.qt_supply_sha256, args.snapshot_manifest, args.expected_manifest_sha256, args.create_root)
        sdk.audit_inputs = [{"path": str(OWNED / name), "sha256": sha256(OWNED / name)} for name in
                            ("linux_d_sdk.py", "linux_sdk.py", "dependency_source.py", "audit_support.py",
                             "guest_command.py", "verify_snapshot.py", "git_source.py")]
        sdk.evidence["supplierInputs"] = sdk.audit_inputs
        with sdk.writer():
            if args.product_source_manifest is not None:
                sdk.bind_git_source(args.product_source_manifest, args.product_source_sha256, args.product_git_commit)
            if args.openssl_supply is not None:
                sdk.bind_openssl_supply(args.openssl_supply, args.openssl_supply_sha256)
            if args.only == "platform":
                if args.platform_index is None:
                    raise AuditSafetyError("Exact platform index required")
                sdk.platform_runtime(args.platform_index, args.previous_platform, args.previous_platform_sha256)
            elif args.only == "openssl":
                if args.openssl_pin is None:
                    raise AuditSafetyError("Exact supplemental stable OpenSSL pin required")
                sdk.openssl(args.openssl_pin, args.previous_openssl, args.previous_openssl_sha256)
            elif args.only == "deps":
                if args.dependencies is None or "curl" in args.dependencies:
                    if getattr(sdk, "openssl_supply", None) is None:
                        raise AuditSafetyError("Curl requires this root's verified supplemental static OpenSSL supply")
                if args.archive_root is not None:
                    sdk.archive_root = plain_path(args.archive_root, must_exist=True)
                source = getattr(sdk, "product_freeze", sdk.freeze)
                sdk.deps(source.manifest, source.expected_sha256, args.dependencies, source_freeze=source)
            elif args.only == "product":
                sdk.product()
            sdk.guard()
            sdk.evidence.update(state="FINISHED", passed=True, finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            sdk.save()
        print(json.dumps({"output": str(sdk.output), "sha256": sha256(sdk.output), "phasePassed": True,
                          "token": sdk.token, "completeProductVerified": False}))
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

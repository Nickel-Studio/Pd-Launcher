#!/usr/bin/env python3
"""Provision an explicitly marked Linux SDK; never accept probes as the product."""
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
import subprocess
import sys
import tarfile
import time

from audit_support import AuditSafetyError, OWNED, atomic_json, owned_output, plain_path, sha256
from guest_command import cancel_descendants, proc_identity
from dependency_source import SourceFreeze

LIMIT = 1536 * 1024 * 1024
RESERVE_C = 1024 * 1024 * 1024
RESERVE_D = 256 * 1024 * 1024
MIRROR = OWNED.parents[1] / ".snow/native-shared"


class Sdk:
    def __init__(self, root: Path, token: str, output: Path):
        if sys.platform != "linux" or not hasattr(os, "pidfd_open"):
            raise AuditSafetyError("An actual Linux pidfd host is required")
        self.root = plain_path(root, must_exist=True)
        marker_path = plain_path(self.root / ".nickel-portability-owned.json", must_exist=True)
        marker = json.loads(marker_path.read_bytes())
        info = self.root.stat()
        if (not isinstance(marker, dict) or marker.get("kind") != "portability-owned-linux-sdk"
                or marker.get("owner") != str(OWNED) or marker.get("root") != str(self.root)
                or marker.get("token") != token or marker.get("uid") != os.getuid()
                or marker.get("device") != info.st_dev or marker.get("inode") != info.st_ino
                or marker.get("limitBytes") != LIMIT):
            raise AuditSafetyError("The SDK marker does not identify this exact owner/root/token")
        self.root_identity = (info.st_dev, info.st_ino, info.st_uid)
        self.marker_bytes = marker_path.read_bytes()
        self.output = owned_output(output)
        if self.output.exists():
            raise AuditSafetyError("Existing SDK evidence is preserved")
        self.prefix = self.root / "prefix"
        self.control = self.output.with_suffix(".control.json")
        self.token = token
        self.evidence = {"kind": "actual-linux-sdk-provisioning", "createdUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
                         "state": "STARTING", "workerPid": os.getpid(), "root": str(self.root), "limitBytes": LIMIT,
                         "markerSha256": hashlib.sha256(self.marker_bytes).hexdigest(), "commands": [], "sources": [],
                         "completeProductVerified": False, "passed": False,
                         "scope": "Real pinned dependencies and official dynamic Qt SDK only; no launcher/UI/release acceptance"}
        self.environment = dict(os.environ)
        self.environment.update(TEMP=str(self.root / "tmp"), TMP=str(self.root / "tmp"),
                                TMPDIR=str(self.root / "tmp"), HOME=str(self.root / "home"),
                                LD_LIBRARY_PATH=str(self.root / "qt/6.8.3/gcc_64/lib") + ":" + str(self.root / "runtime/usr/lib/x86_64-linux-gnu"),
                                PKG_CONFIG_PATH=str(self.prefix / "lib/pkgconfig"),
                                PYTHONDONTWRITEBYTECODE="1", CMAKE_BUILD_PARALLEL_LEVEL="1")
        self.guard()
        plain_path(self.root / "home", root=self.root).mkdir(exist_ok=True, mode=0o700)
        self.guard()
        self.save()

    def guard(self) -> dict:
        info = plain_path(self.root, must_exist=True).stat()
        if (info.st_dev, info.st_ino, info.st_uid) != self.root_identity:
            raise AuditSafetyError("SDK root identity changed")
        marker = self.root / ".nickel-portability-owned.json"
        if plain_path(marker, must_exist=True).read_bytes() != self.marker_bytes:
            raise AuditSafetyError("SDK ownership marker changed")
        freeze = getattr(self, "freeze", None)
        if freeze is not None:
            freeze.check()
        logical = allocated = count = 0
        for directory, folders, files in os.walk(self.root, followlinks=False):
            for name in [*folders, *files]:
                info = (Path(directory) / name).lstat()
                allocated += info.st_blocks * 512
                if not stat.S_ISDIR(info.st_mode):
                    logical += info.st_size
                count += 1
        charged = max(logical, allocated)
        free_c = shutil.disk_usage("/mnt/c").free
        free_d = shutil.disk_usage("/mnt/d").free
        result = {"logicalBytes": logical, "allocatedBytes": allocated, "chargedBytes": charged,
                  "entryCount": count, "freeCBytes": free_c, "freeDBytes": free_d}
        self.evidence["latestCapacity"] = result
        if charged > LIMIT or free_c < RESERVE_C or free_d < RESERVE_D:
            raise AuditSafetyError(f"SDK quota/reserve: charged={charged}, C={free_c}, D={free_d}")
        return result

    @contextmanager
    def writer(self):
        self.guard()
        lock = plain_path(self.root / ".sdk-writer.lock", root=self.root)
        with lock.open("a+b") as handle:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)

    def save(self):
        atomic_json(self.output, self.evidence)

    def command(self, argv: list[str], *, cwd: Path | None = None, seconds: int = 1200, env: dict | None = None):
        self.guard()
        index = len(self.evidence["commands"]) + 1
        log = owned_output(self.output.with_name(self.output.stem + f".step-{index:02d}.log"))
        record = {"argv": argv, "cwd": str(cwd or self.root), "startedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
                  "state": "STARTING", "exitCode": None, "log": str(log), "passed": False}
        self.evidence["commands"].append(record)
        self.evidence["state"] = "RUNNING"
        self.save()
        process = None
        started = time.monotonic()
        try:
            with log.open("xb") as stream:
                process = subprocess.Popen(argv, cwd=cwd or self.root, env=env or self.environment,
                                           stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                record.update(state="RUNNING", child=proc_identity(process.pid))
                self.save()
                while process.poll() is None:
                    self.guard()
                    if log.stat().st_size > 16 * 1024 * 1024:
                        raise AuditSafetyError("SDK step log exceeded 16 MiB")
                    # A DrvFS inventory can outlive the child; do not turn its completed exit into a timeout.
                    if process.poll() is not None:
                        break
                    if time.monotonic() - started >= seconds:
                        raise AuditSafetyError("SDK step timed out")
                    if self.control.exists():
                        control = json.loads(self.control.read_bytes())
                        if control.get("token") != self.token:
                            raise AuditSafetyError("SDK control does not identify this owner")
                        if control.get("action") == "cancel":
                            raise AuditSafetyError("SDK exact owned step cancelled")
                    time.sleep(0.5)
                record.update(state="FINISHED", exitCode=process.wait(timeout=15))
            record["passed"] = record["exitCode"] == 0
            if not record["passed"]:
                raise AuditSafetyError("SDK command failed; raw output preserved at " + str(log))
        except BaseException as error:
            record["error"] = str(error)
            if process is not None:
                if process.poll() is None:
                    record["cancelledExactProcesses"] = cancel_descendants(process)
                    record.update(state="CANCELLED_EXACT_OWNED_STEP", exitCode=process.wait(timeout=15))
                elif record["state"] != "FINISHED":
                    record.update(state="FAILED_OWNED_STEP", exitCode=process.wait(timeout=15))
            else:
                record["state"] = "START_FAILED"
            raise
        finally:
            record.update(finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                          elapsedSeconds=round(time.monotonic() - started, 3),
                          logSha256=sha256(log) if log.is_file() else None)
            self.save()

    def qt(self, base: str = "https://download.qt.io", archives: list[str] | None = None):
        selected = ["qtbase", "qtsvg", "icu"] if archives is None else archives
        if not selected or len(set(selected)) != len(selected) or any(name not in {"qtbase", "qtsvg", "icu"} for name in selected):
            raise AuditSafetyError("Only the required Qt Widgets/Svg/ICU archives may be installed")
        config = self.root / "aqt.ini"
        downloads = self.root / "downloads"
        expected = ("[aqt]\nconcurrency: 1\nbaseurl: https://download.qt.io\nalways_keep_archives: True\n"
                    f"archive_download_location: {downloads}\n[requests]\nconnection_timeout: 15\nresponse_timeout: 90\n"
                    "max_retries_on_connection_error: 2\nmax_retries_on_checksum_error: 2\nhash_algorithm: sha256\n"
                    "INSECURE_NOT_FOR_PRODUCTION_ignore_hash: False\n")
        self.guard()
        if config.exists():
            if plain_path(config, root=self.root).read_text(encoding="ascii") != expected:
                raise AuditSafetyError("An altered Qt installer configuration is preserved")
        else:
            config.write_text(expected, encoding="ascii")
        downloads.mkdir(mode=0o700, exist_ok=True)
        env = dict(self.environment, PYTHONPATH=str(self.root / "aqt-env"), AQT_CONFIG=str(config))
        # A mirror changes download location, never the official digest authority.
        self.evidence["qtDownloadBase"] = base
        self.evidence["qtSelectedArchives"] = selected
        argv = [sys.executable, "-m", "aqt", "install-qt", "linux", "desktop", "6.8.3", "linux_gcc_64",
                "--base", base, "--archives", *selected]
        if "qtbase" in selected:
            argv += ["--modules", "qtimageformats"]
        self.command(argv + ["--outputdir", str(self.root / "qt"), "--keep", "--archive-dest", str(downloads), "--internal"], env=env)
        qt = self.root / "qt/6.8.3/gcc_64"
        required = [qt / "lib/cmake" / name / (name + "Config.cmake")
                    for name in ("Qt6", "Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Svg", "Qt6Test")]
        if any(not path.is_file() for path in required):
            raise AuditSafetyError("Official Qt installation lacks required exact component configuration")
        self.evidence["qt"] = {"version": "6.8.3", "prefix": str(qt), "baseline": "Official RHEL 8.10 GCC x64 dynamic SDK on Ubuntu 24.04",
                               "sdkOnly": True, "runtimeForbiddenModulesNotYetAudited": True,
                               "archives": [{"path": str(path), "bytes": path.stat().st_size, "sha256": sha256(path)}
                                            for path in sorted(downloads.glob("*.7z"))],
                               "configs": [{"path": str(path), "sha256": sha256(path)} for path in required]}
        self.command([str(qt / "bin/qmake"), "-query", "QT_VERSION"])

    def platform_runtime(self, index_path: Path):
        index_path = owned_output(index_path)
        raw = index_path.read_bytes()
        envelope = json.loads(raw)
        if not envelope.get("command", {}).get("passed"):
            raise AuditSafetyError("The platform runtime index command did not pass")
        index = json.loads(envelope["command"]["stdout"])
        packages = index.get("packages", [])
        names = {"libxkbcommon-x11-0", "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0",
                 "libxcb-keysyms1", "libxcb-render-util0", "libxcb-xkb1", "libxcb-util1"}
        if index.get("exitCode") != 0 or len(packages) != len(names) or {row.get("Package") for row in packages} != names:
            raise AuditSafetyError("The platform runtime index is not the exact required xcb package set")
        runtime = plain_path(self.root / "runtime", root=self.root)
        downloads = plain_path(self.root / "runtime-downloads", root=self.root)
        if runtime.exists() or downloads.exists():
            raise AuditSafetyError("An existing runtime supply is preserved")
        self.guard()
        runtime.mkdir(mode=0o700)
        downloads.mkdir(mode=0o700)
        self.evidence["platformRuntime"] = {"indexPath": str(index_path), "indexSha256": hashlib.sha256(raw).hexdigest(),
                                            "prefix": str(runtime), "systemPackagesUnchanged": True, "packages": []}
        for row in packages:
            if row.get("Architecture") != "amd64" or len(row.get("SHA256", "")) != 64:
                raise AuditSafetyError("Unexpected platform package architecture or digest")
            self.command(["apt-get", "download", row["Package"] + "=" + row["Version"]], cwd=downloads, seconds=120)
            archive = plain_path(downloads / Path(row["Filename"]).name, root=self.root, must_exist=True)
            if archive.stat().st_size != int(row["Size"]) or sha256(archive) != row["SHA256"]:
                raise AuditSafetyError("The runtime package differs from the signed apt index")
            self.evidence["platformRuntime"]["packages"].append({**row, "archivePath": str(archive)})
            self.command(["dpkg-deb", "--extract", str(archive), str(runtime)], seconds=30)
        self.command(["ldd", str(self.root / "qt/6.8.3/gcc_64/plugins/platforms/libqxcb.so")], seconds=30)
        output = Path(self.evidence["commands"][-1]["log"]).read_text(encoding="utf-8")
        if "not found" in output:
            raise AuditSafetyError("The actual xcb plugin runtime still has unresolved libraries")
        self.evidence["platformRuntime"]["inventory"] = [
            {"path": str(path.relative_to(runtime)), "bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in sorted(runtime.rglob("*")) if path.is_file() and not path.is_symlink()]
        self.save()

    def source(self, entry: dict) -> Path:
        archive_root = plain_path(getattr(self, "archive_root", Path("/mnt/d/workon/nickel-native-tools/downloads")),
                                  must_exist=True)
        if not archive_root.is_dir():
            raise AuditSafetyError("The explicit read-only archive root must be a plain directory")
        archive = plain_path(archive_root / entry["archive"], root=archive_root, must_exist=True)
        if sha256(archive) != entry["sha256"]:
            raise AuditSafetyError("Read-only dependency archive differs from lock: " + entry["name"])
        source = plain_path(self.root / "sources" / entry["sourceDirectory"], root=self.root)
        if source.exists():
            raise AuditSafetyError("Existing dependency source is preserved: " + str(source))
        with tarfile.open(archive) as bundle:
            members = bundle.getmembers()
            expanded = sum(member.size for member in members)
            if self.guard()["chargedBytes"] + expanded + 8 * 1024 * 1024 > LIMIT:
                raise AuditSafetyError("Dependency expansion would exceed SDK quota")
            for member in members:
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    raise AuditSafetyError("Archive contains an escaping member")
            bundle.extractall(self.root / "sources", members=members, filter="data")
        if not source.is_dir():
            raise AuditSafetyError("Locked dependency archive root is absent")
        self.evidence["sources"].append({**entry, "archiveRoot": str(archive_root), "archivePath": str(archive),
                                         "sourcePath": str(source), "expandedBytes": expanded})
        self.save()
        return source

    def cmake(self, name: str, source: Path, definitions: list[str]):
        build = self.root / "build" / name
        if build.exists():
            raise AuditSafetyError("Existing SDK build is preserved: " + str(build))
        self.guard()
        build.mkdir(mode=0o700)
        self.command(["cmake", "--debug-trycompile", "-S", str(source), "-B", str(build), "-G", "Ninja",
                      "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + str(self.prefix),
                      "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_PREFIX_PATH=" + str(self.prefix),
                      "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
                      "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON", *definitions])
        self.command(["cmake", "--build", str(build), "--parallel", "1"])
        self.command(["cmake", "--install", str(build)])
        cache = build / "CMakeCache.txt"
        self.evidence.setdefault("buildCaches", []).append({"path": str(cache), "sha256": sha256(cache)})
        self.save()

    def deps(self, manifest: Path, expected_sha256: str, selected: list[str] | None = None, *, source_freeze=None):
        self.guard()
        freeze = SourceFreeze(manifest, expected_sha256) if source_freeze is None else source_freeze
        if freeze.manifest != manifest or freeze.expected_sha256 != expected_sha256:
            raise AuditSafetyError("Dependency source binding differs from its explicit manifest pin")
        freeze.check()
        if source_freeze is None:
            self.freeze = freeze
            self.evidence["dependencySource"] = freeze.evidence
        self.evidence["dependencyBuildSource"] = freeze.evidence
        mirror = freeze.root
        lock = plain_path(mirror / "scripts/native/deps-lock.json", root=mirror, must_exist=True)
        raw = lock.read_bytes()
        entries = {entry["name"]: entry for entry in json.loads(raw)["sources"]}
        self.evidence["dependencyLock"] = {"path": str(lock), "sha256": hashlib.sha256(raw).hexdigest(),
                                            "sourceCount": len(entries)}
        order = ("json", "zlib", "xz", "zstd", "bzip2", "curl", "libarchive", "gumbo", "tomlplusplus", "yaml")
        names = list(order) if selected is None else selected
        if not names or len(set(names)) != len(names) or any(name not in order for name in names):
            raise AuditSafetyError("Supply requires distinct pinned std dependencies")
        self.evidence["selectedDependencies"] = names
        for name in names:
            source = self.source(entries[name])
            definitions = []
            if name == "json":
                definitions = ["-DJSON_BuildTests=OFF", "-DJSON_MultipleHeaders=ON"]
            elif name == "zlib":
                definitions = ["-DZLIB_BUILD_EXAMPLES=OFF"]
            elif name == "xz":
                definitions = ["-DBUILD_TESTING=OFF", "-DXZ_TOOL_XZ=OFF", "-DXZ_TOOL_XZDEC=OFF", "-DXZ_TOOL_LZMADEC=OFF", "-DXZ_TOOL_LZMAINFO=OFF", "-DXZ_NLS=OFF"]
            elif name == "zstd":
                source = source / "build/cmake"
                definitions = ["-DZSTD_BUILD_SHARED=OFF", "-DZSTD_BUILD_STATIC=ON", "-DZSTD_BUILD_PROGRAMS=OFF", "-DZSTD_BUILD_TESTS=OFF"]
            elif name == "bzip2":
                definitions = ["-DBZIP2_SOURCE_DIR=" + str(source)]
                source = mirror / "scripts/native/deps/bzip2"
            elif name == "curl":
                definitions = ["-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_STATIC_LIBS=ON", "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_USE_LIBSSH=OFF", "-DCURL_USE_GSSAPI=OFF", "-DCURL_USE_LIBIDN2=OFF", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON", "-DHTTP_ONLY=ON", "-DCURL_USE_OPENSSL=ON", "-DOPENSSL_USE_STATIC_LIBS=ON", "-DZLIB_LIBRARY=" + str(self.prefix / "lib/libz.a"), "-DZLIB_INCLUDE_DIR=" + str(self.prefix / "include")]
                if getattr(self, "openssl_root", None) is not None:
                    definitions += ["-DOPENSSL_ROOT_DIR=" + str(self.openssl_root),
                                    "-DOPENSSL_INCLUDE_DIR=" + str(self.openssl_root / "include"),
                                    "-DOPENSSL_SSL_LIBRARY=" + str(self.openssl_root / "lib/libssl.a"),
                                    "-DOPENSSL_CRYPTO_LIBRARY=" + str(self.openssl_root / "lib/libcrypto.a")]
            elif name == "libarchive":
                definitions = ["-DENABLE_TEST=OFF", "-DENABLE_TAR=OFF", "-DENABLE_CPIO=OFF", "-DENABLE_CAT=OFF", "-DENABLE_UNZIP=OFF", "-DENABLE_OPENSSL=OFF", "-DENABLE_MBEDTLS=OFF", "-DENABLE_NETTLE=OFF", "-DENABLE_LIBXML2=OFF", "-DENABLE_EXPAT=OFF", "-DENABLE_LZ4=OFF", "-DENABLE_BZip2=ON", "-DENABLE_LZMA=ON", "-DENABLE_ZSTD=ON", "-DENABLE_INSTALL=ON", "-DZLIB_LIBRARY=" + str(self.prefix / "lib/libz.a"), "-DZLIB_INCLUDE_DIR=" + str(self.prefix / "include"), "-DBZIP2_LIBRARIES=" + str(self.prefix / "lib/libbz2.a"), "-DBZIP2_INCLUDE_DIR=" + str(self.prefix / "include"), "-DLIBLZMA_LIBRARY=" + str(self.prefix / "lib/liblzma.a"), "-DLIBLZMA_INCLUDE_DIR=" + str(self.prefix / "include"), "-DZSTD_LIBRARY=" + str(self.prefix / "lib/libzstd.a"), "-DZSTD_INCLUDE_DIR=" + str(self.prefix / "include")]
            elif name == "gumbo":
                definitions = ["-DGUMBO_SOURCE_DIR=" + str(source)]
                source = mirror / "scripts/native/deps/gumbo"
            elif name == "tomlplusplus":
                definitions = ["-DBUILD_EXAMPLES=OFF", "-DTOML_BUILD_TESTS=OFF"]
            elif name == "yaml":
                definitions = ["-DLIBYAML_SOURCE_DIR=" + str(source)]
                source = mirror / "scripts/native/deps/yaml"
            self.cmake(name, source, definitions)
        freeze.postcheck()
        self.guard()
        if lock.read_bytes() != raw:
            raise AuditSafetyError("The pinned dependency lock changed during build")
        self.evidence["prefixInventory"] = [{"path": str(path.relative_to(self.prefix)), "bytes": path.stat().st_size, "sha256": sha256(path)}
                                            for path in sorted(self.prefix.rglob("*")) if path.is_file() and not path.is_symlink()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--token", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--only", required=True, choices=("qt", "platform", "deps"))
    parser.add_argument("--qt-base", default="https://download.qt.io")
    parser.add_argument("--qt-archives", nargs="+", choices=("qtbase", "qtsvg", "icu"))
    parser.add_argument("--platform-index", type=Path)
    parser.add_argument("--snapshot-manifest", type=Path)
    parser.add_argument("--expected-manifest-sha256")
    args = parser.parse_args()
    if args.only == "deps" and (args.snapshot_manifest is None or args.expected_manifest_sha256 is None):
        parser.error("Dependency supply requires --snapshot-manifest and --expected-manifest-sha256")
    sdk = None
    try:
        sdk = Sdk(args.root, args.token, args.output)
        with sdk.writer():
            if args.only == "qt":
                sdk.qt(args.qt_base, args.qt_archives)
            elif args.only == "platform":
                if args.platform_index is None:
                    raise AuditSafetyError("The exact owned platform package index is required")
                sdk.platform_runtime(args.platform_index)
            else:
                sdk.deps(args.snapshot_manifest, args.expected_manifest_sha256)
            sdk.guard()
            sdk.evidence.update(state="FINISHED", passed=True, finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            sdk.save()
            print(json.dumps({"output": str(sdk.output), "sha256": sha256(sdk.output), "sdkPhasePassed": True, "completeProductVerified": False}))
            return 0
    except BaseException as error:
        if sdk is not None:
            sdk.evidence.update(state="FAILED_OR_BLOCKED", error=str(error), passed=False,
                                finishedUtc=dt.datetime.now(dt.timezone.utc).isoformat())
            sdk.save()
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

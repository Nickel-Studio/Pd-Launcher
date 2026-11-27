#!/usr/bin/env python3
"""Build pinned static native dependencies in a single-writer external tools root.

Versions, archive hashes and source-tree digests come from deps-lock.json; any mismatch
fails instead of silently reusing or downloading a different copy.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

from msvc import DEFAULT_VS, developer_environment

HERE = Path(__file__).resolve().parent


SOURCE_MIRRORS = {
    # 主站返回的字节与锁定哈希不一致时，改用官方发布页的同名归档（与锁定哈希一致）。
    "zlib-1.3.1.tar.gz": [
        "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
    ],
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_tree_digest(root: Path) -> str:
    rows = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("Bundled source must not contain links")
        if path.is_file():
            rows.append({"path": path.relative_to(root).as_posix(), "sha256": digest(path)})
    rows.sort(key=lambda row: row["path"])
    return hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def require_free_space(path: Path, minimum: int):
    existing = path
    while not existing.exists():
        existing = existing.parent
    if shutil.disk_usage(existing).free < minimum:
        raise RuntimeError(f"Insufficient free space before writing {path}: require {minimum} bytes")



@contextmanager
def exclusive_build(root: Path):
    lock = root / ".build-lock"
    with lock.open("a+b") as handle:
        handle.seek(0)
        handle.write(b"0")
        handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


class Builder:
    def __init__(self, options: argparse.Namespace):
        self.options = options
        self.root = options.tools_root.resolve()
        self.work_root = (options.work_root or self.root).resolve()
        self.build_root = (options.build_root or self.work_root / "build").resolve()
        self.manifest = json.loads((HERE / "deps-lock.json").read_text(encoding="utf-8"))
        self.sources = {entry["name"]: entry for entry in self.manifest["sources"]}
        self.target = options.target or ("windows-x64-static" if os.name == "nt" else f"{platform.system().lower()}-{platform.machine()}-static")
        self.prefix = self.root / "install" / self.target
        self.check_space()
        self.environment = developer_environment(options.vs_root)
        self.cmake = shutil.which("cmake", path=self.environment["PATH"])
        self.ninja = shutil.which("ninja", path=self.environment["PATH"])
        if not self.cmake or not self.ninja:
            raise RuntimeError("CMake and Ninja must be installed")
        self.root.mkdir(parents=True, exist_ok=True)
        self.build_root.mkdir(parents=True, exist_ok=True)
        for child in ("downloads", "sources", "logs"):
            (self.work_root / child).mkdir(parents=True, exist_ok=True)
        self.log = self.work_root / "logs" / f"bootstrap-{self.target}-{options.only}.log"
        self.evidence = {"schema": 1, "startedUtc": dt.datetime.now(dt.UTC).isoformat(), "target": self.target, "toolsRoot": str(self.root), "workRoot": str(self.work_root), "buildRoot": str(self.build_root), "prefix": str(self.prefix), "lockSha256": digest(HERE / "deps-lock.json"), "commands": [], "sources": [], "complete": False}

    def check_space(self):
        work_minimum = (512 * 1024 ** 2) if self.options.only == "imageformats" else (4 * 1024 ** 3)
        install_minimum = (64 * 1024 ** 2) if self.options.only == "imageformats" else (4 * 1024 ** 3)
        require_free_space(self.root, install_minimum)
        require_free_space(self.work_root, work_minimum)
        require_free_space(self.build_root, work_minimum)

    def command(self, arguments: list[str], cwd: Path | None = None):
        cwd = cwd or self.root
        record = {"argv": arguments, "cwd": str(cwd), "startedUtc": dt.datetime.now(dt.UTC).isoformat()}
        self.evidence["commands"].append(record)
        print("RUN " + json.dumps(arguments), flush=True)
        with self.log.open("a", encoding="utf-8") as log:
            log.write("\nRUN " + json.dumps(record) + "\n")
            log.flush()
            result = subprocess.run(arguments, cwd=cwd, env=self.environment, stdout=log, stderr=subprocess.STDOUT)
        record["returnCode"] = result.returncode
        record["finishedUtc"] = dt.datetime.now(dt.UTC).isoformat()
        self.save_evidence()
        if result.returncode:
            raise RuntimeError(f"Command failed ({result.returncode}); see {self.log}")

    def save_evidence(self):
        path = self.work_root / "logs" / f"bootstrap-{self.target}-{self.options.only}.json"
        temporary = path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(self.evidence, indent=2) + "\n", encoding="utf-8")
        temporary.replace(path)

    def source(self, name: str) -> Path:
        entry = self.sources[name]
        archive = self.work_root / "downloads" / entry["archive"]
        if not archive.is_file() or digest(archive) != entry["sha256"]:
            temporary = archive.with_suffix(archive.suffix + ".part")
            candidates = [entry["url"]] + SOURCE_MIRRORS.get(entry["archive"], [])
            curl = shutil.which("curl.exe" if os.name == "nt" else "curl")
            for url in candidates:
                if not url.startswith("https://"):
                    raise ValueError("Dependency source must use HTTPS")
                print("DOWNLOAD " + url, flush=True)
                temporary.unlink(missing_ok=True)
                if curl:
                    self.command([curl, "--fail", "--location", "--retry", "3", "--connect-timeout", "20", "--max-time", "900", "--output", str(temporary), url])
                else:
                    request = urllib.request.Request(url, headers={"User-Agent": "Nickel-native-bootstrap/1"})
                    with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
                        shutil.copyfileobj(response, output)
                if digest(temporary) == entry["sha256"]:
                    break
                print("SOURCE HASH RETRY " + entry["archive"] + " from " + url, flush=True)
            else:
                temporary.unlink(missing_ok=True)
                raise RuntimeError("Source hash mismatch: " + entry["archive"])
            temporary.replace(archive)
        source = self.work_root / "sources" / entry["sourceDirectory"]
        stamp = source / ".nickel-source-sha256"
        if not stamp.is_file():
            if source.exists():
                if name != "qtbase" or not (source / "configure.bat").is_file():
                    raise RuntimeError("Unmarked dependency source already exists: " + str(source))
            else:
                print("EXTRACT " + archive.name, flush=True)
                with tarfile.open(archive) as bundle:
                    bundle.extractall(self.work_root / "sources", filter="data")
            if not source.is_dir():
                raise RuntimeError("Unexpected archive root: " + entry["archive"])
            stamp.write_text(entry["sha256"] + "\n", encoding="ascii")
        elif stamp.read_text(encoding="ascii").strip() != entry["sha256"]:
            raise RuntimeError("Dependency source stamp changed: " + name)
        for bundled in entry.get("bundledSources", []):
            directory = source / bundled["sourceDirectory"]
            attribution = directory / bundled["versionEvidence"]
            version = json.loads(attribution.read_text(encoding="utf-8"))
            references = {bundled["licenseFile"]: bundled["licenseSha256"],
                          bundled["versionEvidence"]: bundled["versionEvidenceSha256"], **bundled["noticeFiles"]}
            if version["Version"] != bundled["version"] or version["LicenseId"] != bundled["license"]:
                raise RuntimeError("Bundled dependency version/license changed: " + bundled["name"])
            if source_tree_digest(directory) != bundled["sourceTreeSha256"]:
                raise RuntimeError("Bundled dependency source tree changed: " + bundled["name"])
            if any(digest(directory / filename) != expected for filename, expected in references.items()):
                raise RuntimeError("Bundled dependency legal material changed: " + bundled["name"])
        self.evidence["sources"].append({**entry, "archivePath": str(archive), "sourcePath": str(source), "verifiedSha256": digest(archive)})
        self.save_evidence()
        return source

    def cmake_project(self, name: str, definitions: list[str], subdirectory: str = "", source_override: Path | None = None,
                      build_targets: tuple[str, ...] = (), install_subdirectory: str = ""):
        self.check_space()
        source = source_override if source_override is not None else self.source(name) / subdirectory
        build = self.build_root / self.target / name
        build.mkdir(parents=True, exist_ok=True)
        arguments = [self.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={self.prefix.as_posix()}", f"-DCMAKE_PREFIX_PATH={self.prefix.as_posix()}", f"-DCMAKE_MAKE_PROGRAM={Path(self.ninja).as_posix()}", "-DBUILD_SHARED_LIBS=OFF", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON"]
        if os.name == "nt":
            arguments += ["-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
        if sys.platform == "darwin":
            arguments += ["-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"]
        self.command(arguments + definitions)
        if name == "qtbase":
            self.verify_qt_cache(build)
        if name == "qtimageformats":
            self.verify_imageformats_cache(build)
        command = [self.cmake, "--build", str(build), "--parallel", str(self.options.jobs)]
        if build_targets:
            command += ["--target", *build_targets]
        self.command(command)
        self.check_space()
        self.command([self.cmake, "--install", str(build / install_subdirectory), "--config", "Release", "--prefix", str(self.prefix)])

    def verify_qt_cache(self, build: Path):
        cache = {}
        for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            if line and not line.startswith(("//", "#")) and "=" in line and ":" in line:
                key, value = line.split("=", 1)
                cache[key.split(":", 1)[0]] = value
        expected = {"QT_FEATURE_static": "ON", "QT_FEATURE_shared": "OFF",
                    "QT_FEATURE_dynamicgl": "OFF", "QT_FEATURE_opengl": "OFF",
                    "QT_FEATURE_network": "OFF", "QT_FEATURE_testlib": "ON",
                    "QT_FEATURE_gui": "ON", "QT_FEATURE_widgets": "ON",
                    "QT_FEATURE_precompile_header": "OFF"}
        if os.name == "nt":
            expected["QT_FEATURE_static_runtime"] = "ON"
        self.evidence["qtFeatures"] = {key: cache.get(key) for key in expected}
        self.save_evidence()
        mismatches = {key: {"expected": value, "actual": cache.get(key)}
                      for key, value in expected.items() if cache.get(key) != value}
        if mismatches:
            raise RuntimeError("Qt computed features differ from release policy: " + json.dumps(mismatches))

    def build_qt(self):
        # Clear both the result cache and its index, or dirty detection overrides explicit inputs.
        definitions = ["-UQT_FEATURE_*", "-UQT_KNOWN_FEATURES", "-DINPUT_static=ON", "-DFEATURE_shared=OFF", "-DFEATURE_static=ON", "-DBUILD_WITH_PCH=OFF", "-DFEATURE_precompile_header=OFF", "-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_TOOLS_BY_DEFAULT=ON", "-DFEATURE_network=OFF", "-DFEATURE_sql=OFF", "-DFEATURE_dbus=OFF", "-DINPUT_opengl=no", "-DFEATURE_opengl=OFF", "-DFEATURE_dynamicgl=OFF", "-DFEATURE_vulkan=OFF", "-DFEATURE_printsupport=OFF", "-DFEATURE_testlib=ON", "-DFEATURE_gui=ON", "-DFEATURE_widgets=ON", "-DFEATURE_concurrent=ON", "-DFEATURE_system_zlib=OFF", "-DFEATURE_system_freetype=OFF", "-DFEATURE_system_harfbuzz=OFF", "-DFEATURE_system_pcre2=OFF", "-DFEATURE_system_doubleconversion=OFF", "-DFEATURE_system_png=OFF", "-DFEATURE_system_jpeg=OFF", "-DFEATURE_system_libb2=OFF"]
        if os.name == "nt":
            definitions += ["-DINPUT_static_runtime=ON", "-DFEATURE_static_runtime=ON"]
        if sys.platform.startswith("linux"):
            definitions = [value for value in definitions if value != "-DFEATURE_dbus=OFF"]
            definitions += ["-DFEATURE_xcb=ON", "-DFEATURE_dbus=ON", "-DFEATURE_accessibility=ON"]
        self.cmake_project("qtbase", definitions)
        self.cmake_project("qtsvg", ["-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF", "-DBUILD_WITH_PCH=OFF", "-DFEATURE_precompile_header=OFF"])

    def verify_imageformats_cache(self, build: Path):
        cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines()
                     if line and not line.startswith(("//", "#")) and "=" in line)
        expected = {"QT_FEATURE_webp:INTERNAL": "ON", "QT_FEATURE_system_webp:INTERNAL": "OFF",
                    "QT_FEATURE_tiff:INTERNAL": "OFF", "QT_FEATURE_mng:INTERNAL": "OFF",
                    "QT_FEATURE_jasper:INTERNAL": "OFF", "BUILD_SHARED_LIBS:BOOL": "OFF"}
        self.evidence["imageformatsFeatures"] = {key: cache.get(key) for key in expected}
        self.save_evidence()
        if any(cache.get(key) != value for key, value in expected.items()):
            raise RuntimeError("QtImageFormats computed features differ from the WebP-only policy")

    def build_imageformats(self):
        self.cmake_project("qtimageformats", ["-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF", "-DBUILD_WITH_PCH=OFF",
                           "-DINPUT_webp=qt", "-DFEATURE_webp=ON", "-DFEATURE_system_webp=OFF",
                           "-DINPUT_tiff=no", "-DFEATURE_tiff=OFF", "-DFEATURE_mng=OFF", "-DFEATURE_jasper=OFF"],
                           build_targets=("QWebpPlugin", "QWebpPlugin_init"),
                           install_subdirectory="src/plugins/imageformats/webp")


    def build_core(self):
        openssl = []
        if os.name != "nt":
            if self.options.openssl_prefix is None:
                raise RuntimeError("Non-Windows core supply requires the explicitly approved static OpenSSL prefix")
            prefix = self.options.openssl_prefix.resolve()
            libraries = [prefix / "lib" / name for name in ("libssl.a", "libcrypto.a")]
            header = prefix / "include/openssl/opensslv.h"
            if not header.is_file() or any(not path.is_file() for path in libraries):
                raise RuntimeError("The supplied OpenSSL prefix lacks its actual headers or static libraries")
            if '# define OPENSSL_VERSION_STR "3.5.9"' not in header.read_text(encoding="utf-8"):
                raise RuntimeError("The supplied OpenSSL headers must match approved version 3.5.9")
            openssl = [f"-DOPENSSL_ROOT_DIR={prefix.as_posix()}",
                       f"-DOPENSSL_INCLUDE_DIR={(prefix / 'include').as_posix()}",
                       f"-DOPENSSL_SSL_LIBRARY={libraries[0].as_posix()}",
                       f"-DOPENSSL_CRYPTO_LIBRARY={libraries[1].as_posix()}",
                       "-DOPENSSL_USE_STATIC_LIBS=ON"]
            self.evidence["supplementalOpenSSL"] = {
                "prefix": str(prefix), "version": "3.5.9", "systemSslUsed": False,
                "inputs": [{"path": str(path), "sha256": digest(path)} for path in [header, *libraries]]}
            self.save_evidence()
        self.cmake_project("json", ["-DJSON_BuildTests=OFF", "-DJSON_MultipleHeaders=ON"])
        self.cmake_project("zlib", ["-DZLIB_BUILD_EXAMPLES=OFF"])
        self.cmake_project("xz", ["-DBUILD_TESTING=OFF", "-DXZ_TOOL_XZ=OFF", "-DXZ_TOOL_XZDEC=OFF", "-DXZ_TOOL_LZMADEC=OFF", "-DXZ_TOOL_LZMAINFO=OFF", "-DXZ_NLS=OFF"])
        self.cmake_project("zstd", ["-DZSTD_BUILD_SHARED=OFF", "-DZSTD_BUILD_STATIC=ON", "-DZSTD_BUILD_PROGRAMS=OFF", "-DZSTD_BUILD_TESTS=OFF", "-DZSTD_USE_STATIC_RUNTIME=ON"], "build/cmake")
        bzip2 = self.source("bzip2")
        self.cmake_project("bzip2", [f"-DBZIP2_SOURCE_DIR={bzip2.as_posix()}"], source_override=HERE / "deps" / "bzip2")
        extension = ".lib" if os.name == "nt" else ".a"
        zlib = self.prefix / "lib" / ("zlibstatic" + extension if os.name == "nt" else "libz.a")
        lzma = self.prefix / "lib" / ("lzma.lib" if os.name == "nt" else "liblzma.a")
        zstd = self.prefix / "lib" / ("zstd_static.lib" if os.name == "nt" else "libzstd.a")
        bz2 = self.prefix / "lib" / ("bz2.lib" if os.name == "nt" else "libbz2.a")
        compression = [f"-DZLIB_LIBRARY={zlib.as_posix()}", f"-DZLIB_INCLUDE_DIR={(self.prefix / 'include').as_posix()}"]
        curl = ["-DBUILD_CURL_EXE=OFF", "-DBUILD_TESTING=OFF", "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_USE_LIBSSH=OFF", "-DCURL_USE_GSSAPI=OFF", "-DCURL_USE_LIBIDN2=OFF", "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON", "-DHTTP_ONLY=ON"]
        if os.name == "nt":
            curl += ["-DCURL_USE_OPENSSL=OFF", "-DCURL_USE_SCHANNEL=ON", "-DCURL_STATIC_CRT=ON"]
        else:
            curl += ["-DCURL_USE_OPENSSL=ON", *openssl]
        self.cmake_project("curl", curl + compression)
        include = (self.prefix / "include").as_posix()
        archive = ["-DENABLE_TEST=OFF", "-DENABLE_TAR=OFF", "-DENABLE_CPIO=OFF", "-DENABLE_CAT=OFF", "-DENABLE_UNZIP=OFF", "-DENABLE_OPENSSL=OFF", "-DENABLE_MBEDTLS=OFF", "-DENABLE_NETTLE=OFF", "-DENABLE_LIBXML2=OFF", "-DENABLE_EXPAT=OFF", "-DENABLE_LZ4=OFF", "-DENABLE_BZip2=ON", "-DENABLE_LZMA=ON", "-DENABLE_ZSTD=ON", "-DENABLE_INSTALL=ON", f"-DBZIP2_LIBRARIES={bz2.as_posix()}", f"-DBZIP2_INCLUDE_DIR={include}", f"-DLIBLZMA_LIBRARY={lzma.as_posix()}", f"-DLIBLZMA_INCLUDE_DIR={include}", f"-DZSTD_LIBRARY={zstd.as_posix()}", f"-DZSTD_INCLUDE_DIR={include}"]
        if os.name == "nt":
            archive += ["-DENABLE_CNG=ON"]
        self.cmake_project("libarchive", archive + compression)

    def build_parsers(self):
        gumbo = self.source("gumbo")
        self.cmake_project("gumbo", [f"-DGUMBO_SOURCE_DIR={gumbo.as_posix()}"], source_override=HERE / "deps" / "gumbo")
        self.cmake_project("tomlplusplus", ["-DBUILD_EXAMPLES=OFF"])
        yaml = self.source("yaml")
        self.cmake_project("yaml-native", [f"-DLIBYAML_SOURCE_DIR={yaml.as_posix()}"], source_override=HERE / "deps" / "yaml")

    def run(self):
        with exclusive_build(self.root):
            self.check_space()
            if self.options.only in ("core", "all"):
                self.build_core()
            if self.options.only in ("parsers", "all"):
                self.build_parsers()
            if self.options.only in ("qt", "all"):
                self.build_qt()
            if self.options.only in ("imageformats", "qt", "all"):
                self.build_imageformats()
            self.evidence["complete"] = True
            self.evidence["finishedUtc"] = dt.datetime.now(dt.UTC).isoformat()
            self.save_evidence()
            print("INSTALLED " + str(self.prefix), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, default=Path(os.environ.get("NICKEL_NATIVE_TOOLS", r"D:\workon\nickel-native-tools" if os.name == "nt" else str(Path.home() / ".cache" / "nickel-native-tools"))))
    parser.add_argument("--work-root", type=Path, help="Private downloads, sources and logs; installed libraries stay in tools-root")
    parser.add_argument("--build-root", type=Path, help="Optional short private build directory; defaults to work-root/build")
    parser.add_argument("--vs-root", type=Path, default=DEFAULT_VS)
    parser.add_argument("--target")
    parser.add_argument("--openssl-prefix", type=Path, help="Approved static OpenSSL 3.5.9 prefix for non-Windows core supply")
    parser.add_argument("--only", choices=("core", "parsers", "qt", "imageformats", "all"), default="all")
    parser.add_argument("--jobs", type=int, default=1)
    options = parser.parse_args()
    if not 1 <= options.jobs <= 4:
        parser.error("Use 1-4 jobs; default 1 protects low-memory build machines")
    builder = None
    try:
        builder = Builder(options)
        builder.run()
    except Exception as error:
        if builder is not None:
            builder.evidence["failure"] = str(error)
            builder.save_evidence()
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

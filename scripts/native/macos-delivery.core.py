#!/usr/bin/env python3
"""Static non-Qt source build recipes; invoked only by the verified Darwin entry."""
from __future__ import annotations

from pathlib import Path
import re
import shutil


def build_core(options, inputs: dict, contract: dict, api: dict) -> dict:
    error = api["DeliveryError"]
    environment, host = api["apple_environment"](inputs["policy"], options.arch)
    verified = api["verify_file_manifest"](options.source_root, options.production_manifest,
                                            options.production_sha256, "Core recipe source")
    source_root = options.source_root.resolve()
    root = options.tools_root.resolve()
    if api["within"](root, source_root) or root.exists():
        raise error("Core build requires a fresh external Apple tools root")
    if shutil.disk_usage(root.parent).free < 4 * 1024 ** 3:
        raise error("At least 4 GiB free required for a static core source build")
    names = ("openssl", "json", "tomlplusplus", "zlib", "xz", "zstd", "bzip2", "gumbo", "yaml", "curl", "libarchive")
    entries = {item["name"]: item for item in contract["sourceRecords"]}
    if not set(names).issubset(entries) or contract.get("supplementSha256") is None:
        raise error("Core source build requires all frozen non-Qt pins and approved OpenSSL supplement")
    archives = api["verify_archives"](inputs, options.downloads, [entries[name] for name in names])
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    if not cmake or not ninja:
        raise error("Verified exact CMake/Ninja must be on PATH")
    recorder = api["Recorder"](options.output, {"kind": "static-core-source-build", "architecture": options.arch,
                                               "host": host, "sourceContract": contract, "archives": archives,
                                               "recipeSourceManifestSha256": verified["manifestSha256"]})
    root.mkdir(parents=True)
    prefix = root / ("core-static-" + options.arch)
    flags = "-O2 -g -fPIC -arch " + options.arch + " -mmacosx-version-min=12.0"
    compiler = api["capture"](["xcrun", "--find", "clang"], environment=environment)
    if compiler.get("returnCode") != 0:
        raise error("Pinned Xcode C compiler could not be resolved")
    environment.update(CC=compiler["stdout"].strip(), CFLAGS=flags, CXXFLAGS=flags,
                       LDFLAGS="-arch " + options.arch + " -mmacosx-version-min=12.0")
    for variable in ("PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "OPENSSL_ROOT_DIR", "OPENSSL_DIR"):
        environment.pop(variable, None)
    environment["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    sources = {}

    def extract(name: str) -> Path:
        entrypoint = {"openssl": "Configure", "zstd": "build/cmake/CMakeLists.txt", "bzip2": "bzlib.h",
                      "gumbo": "src/gumbo.h", "yaml": "include/yaml.h"}.get(name, "CMakeLists.txt")
        source = api["extract_verified_archive"](options.downloads / entries[name]["archive"], entries[name],
                                                  root / ("source-" + name), entrypoint)
        sources[name] = source
        return source

    def project(name: str, definitions: list[str], subdirectory: str = "", wrapper: str | None = None):
        source = extract(name)
        if wrapper:
            relative = "scripts/native/deps/" + wrapper + "/CMakeLists.txt"
            if relative not in verified["paths"]:
                raise error("Dependency wrapper is absent from the actual source manifest")
            project_root = source_root / "scripts/native/deps" / wrapper
            text = api["checked_file"](source_root, relative).read_text(encoding="utf-8-sig")
            source_options = {option for option in re.findall(r'\$\{([A-Z][A-Z0-9_]*SOURCE_(?:DIR|ROOT))\}', text) if not option.startswith("CMAKE_")}
            if len(source_options) != 1:
                raise error("Actual dependency wrapper must declare one explicit verified source path option")
            definitions = ["-D" + source_options.pop() + "=" + str(source)]
        elif name == "zlib":
            relative = "tests/native-macos/deps/zlib/CMakeLists.txt"
            if relative not in verified["paths"]:
                raise error("Owned static zlib recipe is not bound to the actual source manifest")
            project_root = source_root / "tests/native-macos/deps/zlib"
        else:
            project_root = source / subdirectory
        definitions = [value.replace("{source}", str(source)) for value in definitions]
        build = root / ("build-" + name)
        common = ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_INSTALL_PREFIX={prefix}",
                  f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DBUILD_SHARED_LIBS=OFF",
                  "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", f"-DCMAKE_OSX_ARCHITECTURES={options.arch}",
                  "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0", f"-DCMAKE_OSX_SYSROOT={environment['SDKROOT']}",
                  f"-DCMAKE_C_COMPILER={environment['CC']}", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
                  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                  "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local", "-DBUILD_TESTING=OFF"]
        recorder.run([cmake, "-S", str(project_root), "-B", str(build), "-G", "Ninja", *common, *definitions], root, environment)
        cache = api["parse_cache"](build / "CMakeCache.txt")
        for key, expected in {"CMAKE_OSX_ARCHITECTURES": options.arch, "CMAKE_OSX_DEPLOYMENT_TARGET": "12.0"}.items():
            if cache.get(key) != expected:
                raise error("Dependency build changed mandatory Apple policy: " + name + "/" + key)
        if name == "curl":
            for key, expected in {"OPENSSL_CRYPTO_LIBRARY": str(prefix / "lib/libcrypto.a"),
                                  "OPENSSL_SSL_LIBRARY": str(prefix / "lib/libssl.a"),
                                  "OPENSSL_INCLUDE_DIR": str(prefix / "include")}.items():
                if cache.get(key) != expected:
                    raise error("curl used an unverified/system OpenSSL location")
        recorder.run([cmake, "--build", str(build), "--parallel", str(options.jobs)], root, environment)
        recorder.run([cmake, "--install", str(build)], root, environment)

    try:
        cmake_version = recorder.run([cmake, "--version"], root, environment, 30)
        ninja_version = recorder.run([ninja, "--version"], root, environment, 30)
        api["verify_tool_output"]("cmake", inputs["tools"]["cmakeVersion"], cmake_version)
        api["verify_tool_output"]("ninja", inputs["tools"]["ninjaVersion"], ninja_version)
        ssl_source = extract("openssl")
        for tool in ("perl", "make"):
            if not shutil.which(tool):
                raise error("Pinned Apple image is missing the OpenSSL build tool " + tool)
            recorder.run([tool, "--version"], root, environment, 30)
        ssl_target = "darwin64-arm64-cc" if options.arch == "arm64" else "darwin64-x86_64-cc"
        recorder.run(["perl", "Configure", ssl_target, "no-shared", "no-tests", "no-module",
                      f"--prefix={prefix}", "--libdir=lib", f"--openssldir={prefix / 'ssl'}"], ssl_source, environment)
        recorder.run(["make", "-j" + str(options.jobs)], ssl_source, environment)
        recorder.run(["make", "install_sw"], ssl_source, environment)
        actual_ssl = recorder.run([str(prefix / "bin/openssl"), "version"], root, environment, 30)
        if not actual_ssl.startswith("OpenSSL 3.5.9 "):
            raise error("Source-built OpenSSL runtime version differs from the supplement")
        project("json", ["-DJSON_BuildTests=OFF", "-DJSON_MultipleHeaders=ON"])
        project("tomlplusplus", ["-DTOMLPLUSPLUS_BUILD_TESTS=OFF", "-DTOMLPLUSPLUS_BUILD_EXAMPLES=OFF"])
        project("zlib", ["-DNICKEL_ZLIB_SOURCE_DIR={source}"])
        project("xz", ["-DXZ_TOOL_XZ=OFF", "-DXZ_TOOL_XZDEC=OFF", "-DXZ_TOOL_LZMADEC=OFF", "-DXZ_TOOL_LZMAINFO=OFF", "-DXZ_NLS=OFF"])
        project("zstd", ["-DZSTD_BUILD_SHARED=OFF", "-DZSTD_BUILD_STATIC=ON", "-DZSTD_BUILD_PROGRAMS=OFF", "-DZSTD_BUILD_TESTS=OFF"], "build/cmake")
        project("bzip2", ["-DBZIP2_SOURCE_DIR={source}"], wrapper="bzip2")
        project("gumbo", ["-DGUMBO_SOURCE_DIR={source}"], wrapper="gumbo")
        project("yaml", ["-DYAML_SOURCE_DIR={source}"], wrapper="yaml")
        compression = [f"-DZLIB_LIBRARY={prefix / 'lib/libz.a'}", f"-DZLIB_INCLUDE_DIR={prefix / 'include'}"]
        project("curl", ["-DBUILD_CURL_EXE=OFF", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_STATIC_LIBS=ON",
                         "-DCURL_USE_LIBPSL=OFF", "-DCURL_USE_LIBSSH2=OFF", "-DCURL_USE_LIBSSH=OFF",
                         "-DCURL_USE_GSSAPI=OFF", "-DCURL_USE_LIBIDN2=OFF", "-DCURL_USE_APPLE_IDN=OFF",
                         "-DCURL_BROTLI=OFF", "-DCURL_ZSTD=OFF", "-DCURL_DISABLE_LDAP=ON", "-DCURL_DISABLE_LDAPS=ON",
                         "-DHTTP_ONLY=ON", "-DCURL_USE_OPENSSL=ON", "-DCURL_USE_SECTRANSP=OFF", "-DOPENSSL_USE_STATIC_LIBS=ON",
                         f"-DOPENSSL_ROOT_DIR={prefix}", f"-DOPENSSL_INCLUDE_DIR={prefix / 'include'}",
                         f"-DOPENSSL_CRYPTO_LIBRARY={prefix / 'lib/libcrypto.a'}", f"-DOPENSSL_SSL_LIBRARY={prefix / 'lib/libssl.a'}", *compression])
        project("libarchive", ["-DENABLE_TEST=OFF", "-DENABLE_TAR=OFF", "-DENABLE_CPIO=OFF", "-DENABLE_CAT=OFF", "-DENABLE_UNZIP=OFF",
                               "-DENABLE_OPENSSL=OFF", "-DENABLE_MBEDTLS=OFF", "-DENABLE_NETTLE=OFF", "-DENABLE_LIBXML2=OFF",
                               "-DENABLE_EXPAT=OFF", "-DENABLE_LZ4=OFF", "-DENABLE_BZip2=ON", "-DENABLE_LZMA=ON", "-DENABLE_ZSTD=ON",
                               "-DENABLE_INSTALL=ON", f"-DBZIP2_LIBRARIES={prefix / 'lib/libbz2.a'}", f"-DBZIP2_INCLUDE_DIR={prefix / 'include'}",
                               f"-DLIBLZMA_LIBRARY={prefix / 'lib/liblzma.a'}", f"-DLIBLZMA_INCLUDE_DIR={prefix / 'include'}",
                               f"-DZSTD_LIBRARY={prefix / 'lib/libzstd.a'}", f"-DZSTD_INCLUDE_DIR={prefix / 'include'}", *compression])
        files, libraries = [], []
        for path in sorted(prefix.rglob("*")):
            if path.is_symlink():
                raise error("Static prefix contains a linked install member")
            if not path.is_file():
                continue
            if path.suffix in (".dylib", ".so") or ".framework" in str(path):
                raise error("Static core unexpectedly installed a dynamic dependency")
            files.append({"path": path.relative_to(prefix).as_posix(), "sha256": api["digest"](path), "bytes": path.stat().st_size})
            if path.suffix == ".a":
                arch = recorder.run(["xcrun", "lipo", "-archs", str(path)], root, environment, 30).strip()
                if arch != options.arch:
                    raise error("Static archive contains a non-native or extra architecture")
                libraries.append({"path": path.relative_to(prefix).as_posix(), "sha256": api["digest"](path), "architecture": arch})
        required = {"lib/libcrypto.a", "lib/libssl.a", "lib/libcurl.a", "lib/libarchive.a", "lib/libz.a", "lib/libbz2.a", "lib/liblzma.a", "lib/libzstd.a", "lib/libgumbo.a", "lib/libyaml.a"}
        if not required.issubset({item["path"] for item in files}):
            raise error("Static core prefix is missing a required canonical installed archive")
        manifest = options.prefix_manifest
        if manifest.exists():
            raise error("Core prefix manifest output must be new")
        api["write_json"](manifest, {"schema": 1, "architecture": options.arch, "deploymentTarget": "12.0",
                                    "sourceContractSha256": options.contract_sha256, "dependencyLockSha256": contract["lockSha256"],
                                    "supplementSha256": contract["supplementSha256"], "files": files})
        recorder.finish(prefix=str(prefix), prefixManifest=str(manifest), prefixManifestSha256=api["digest"](manifest),
                        libraries=libraries, sourceDirectories={name: str(path) for name, path in sources.items()},
                        sourceArchiveHashesVerified=True, systemOpenSslUsed=False, productDelivered=False)
    except Exception as exception:
        recorder.record["failure"] = str(exception)
        recorder.save()
        raise
    return recorder.record

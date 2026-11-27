#!/usr/bin/env python3
"""Read-only verification of the third-party library legal-binding matrix.

Verifies the machine-readable binding matrix for the 40 installed third-party
libraries (lib/ + plugins/ *.lib of the static Qt prefix, matching the
installed-library inventory in docs/native/licenses/native-release-gaps-current.json)
against fact sources that live in this repository:

  * scripts/native/deps-lock.json
        pinned versions + SHA-256 of every source archive (13 components
        including the libwebp bundled inside qtimageformats);
  * docs/native/licenses/trusted-launcher-notice-input.json
        the trusted launcher notice manifest; every cited license text must be
        covered by one of its notice rows;
  * scripts/native/library-binding-evidence.json
        binary-level link evidence for every installed .lib: MSVC link-map
        symbol count / contribution bytes, installed-archive bytes + SHA-256
        and an evidence grade (link-map-proved | justified-absent | gap);
  * per-row in-repo build records
        scripts/native/bootstrap-deps.py targets, scripts/native/deps/* CMake
        wrappers and the native/app/CMakeLists.txt static-Qt consumer contract.

Three gap classes are counted and reported:

  missing-version       the row has no recorded version evidence, or the cited
                        evidence file/field is unreadable or empty.  Version
                        evidence that lives outside the repository (the Qt
                        install-tree SPDX SBOM) is NOT hard-coded here: pass
                        ``--sbom <path>`` to resolve the rows listed in
                        library-binding-evidence.json:sbomVersionMap.  Without
                        ``--sbom`` those rows keep reporting a gap;
  missing-license-text  a cited license text is absent/empty under
                        docs/native/licenses/, or is not covered by the trusted
                        notice manifest;
  missing-binding       the row lacks a complete in-repo build record (script +
                        source + options), or its binary link evidence is not
                        ``link-map-proved`` and not a reason-bearing
                        ``justified-absent``.  A ``justified-absent`` grade with
                        an empty reason still counts as a real gap.

Evidence grades (definitions also stored in the data file itself):

  link-map-proved    declared in the build.ninja LINK_LIBRARIES edge and
                     attributed >=1 symbol / >0 bytes in the production MSVC
                     link map (/MAP);
  justified-absent   installed in the static prefix but provably never linked
                     (0 map symbols / 0 bytes) for a reason recorded per row
                     and aligned with the link-map analysis sections 6a-6d;
                     requires a non-empty reason plus a sourceReport +
                     sourceSection citation;
  gap                no binary evidence, or evidence structure/citation
                     incomplete.  Always counted as a missing-binding gap.

Field-ownership correction: the previous hint text attributed
``finalExecutableUseVerified=false`` and the ``unverifiedGates`` gate array to
docs/native/licenses/native-release-gaps-current.json.  That key does not exist
there.  The field lives in docs/native/licenses/native-dependency-evidence.json
(and the trusted notice input); ``unverifiedGates`` lives in
native-dependency-evidence.json / legal-tooling-evidence.json /
asset-bundle-evidence.json; the gaps JSON instead carries
``nativeReleaseMissingMaterials``.  The corrected mapping is emitted in the
report's ``gateFieldOwners`` section.

The script is strictly read-only: it never writes, never modifies the trusted
notice inputs, and performs no network access. Exit codes: report mode always
exits 0; ``--check`` exits 1 when any gap exists, 0 otherwise.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path
import sys
from typing import Any

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent

LOCK_PATH = "scripts/native/deps-lock.json"
NOTICE_MANIFEST_PATH = "docs/native/licenses/trusted-launcher-notice-input.json"
NOTICE_INPUT_PREFIX = "docs/native/licenses/trusted-launcher-notice-inputs/"
GAP_REPORT_PATH = "docs/native/licenses/native-release-gaps-current.json"
EVIDENCE_PATH = "scripts/native/library-binding-evidence.json"

LICENSES = NOTICE_INPUT_PREFIX + "LICENSES/"

_REQUIRED_ROW_KEYS = ("artifact", "upstream", "licenseId", "licenseTexts", "linkage")

# Shared in-repo license text bundles. Every path below was verified to exist,
# be non-empty and be covered by the trusted notice manifest rows.
_QT_MODULE_LICENSES = [
    LICENSES + "qtbase/LICENSES/LGPL-3.0-only.txt",
    LICENSES + "qtbase/LICENSES/GPL-2.0-only.txt",
    LICENSES + "qtbase/LICENSES/GPL-3.0-only.txt",
    LICENSES + "qtbase/LICENSES/LicenseRef-Qt-Commercial.txt",
]
_QTSVG_MODULE_LICENSES = [
    LICENSES + "qtsvg/LICENSES/LGPL-3.0-only.txt",
    LICENSES + "qtsvg/LICENSES/GPL-2.0-only.txt",
    LICENSES + "qtsvg/LICENSES/GPL-3.0-only.txt",
    LICENSES + "qtsvg/LICENSES/LicenseRef-Qt-Commercial.txt",
]
_QTIMAGEFORMATS_MODULE_LICENSES = [
    LICENSES + "qtimageformats/LICENSES/LGPL-3.0-only.txt",
    LICENSES + "qtimageformats/LICENSES/GPL-2.0-only.txt",
    LICENSES + "qtimageformats/LICENSES/GPL-3.0-only.txt",
    LICENSES + "qtimageformats/LICENSES/LicenseRef-Qt-Commercial.txt",
]
_LIBWEBP_LICENSES = [
    LICENSES + "qtimageformats/src/3rdparty/libwebp/COPYING",
    LICENSES + "qtimageformats/src/3rdparty/libwebp/PATENTS",
]

_MISSING_LINK_EVIDENCE_REASON = (
    "缺少二进制级链接证据：该行未登记 link map 归因记录，也没有经证明的不链入理由"
    "（证据等级缺失或 gap）"
)

# Correct field ownership for the "final executable closure" gate.  The old
# hint text pointed at native-release-gaps-current.json, which has neither
# ``unverifiedGates`` nor ``finalExecutableUseVerified``; that JSON instead
# carries ``nativeReleaseMissingMaterials``.
_GATE_FIELD_OWNERS = {
    "finalExecutableUseVerified": [
        "docs/native/licenses/native-dependency-evidence.json",
        "docs/native/licenses/trusted-launcher-notice-input.json",
    ],
    "unverifiedGates": [
        "docs/native/licenses/native-dependency-evidence.json",
        "docs/native/licenses/legal-tooling-evidence.json",
        "docs/native/licenses/asset-bundle-evidence.json",
    ],
    "nativeReleaseMissingMaterials": [
        "docs/native/licenses/native-release-gaps-current.json",
    ],
    "correctedFrom": (
        "docs/native/licenses/native-release-gaps-current.json unverifiedGates"
        "（该键不存在，旧提示文本归属错位）"
    ),
}

_GRADE_PROVED = "link-map-proved"
_GRADE_JUSTIFIED_ABSENT = "justified-absent"
_GRADE_GAP = "gap"
_GRADES = (_GRADE_PROVED, _GRADE_JUSTIFIED_ABSENT, _GRADE_GAP)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

_QTBASE_BUILD_FILES = [
    "scripts/native/bootstrap-deps.py",
    "scripts/native/deps-lock.json",
    "native/app/CMakeLists.txt",
]
_DEP_BUILD_FILES = [
    "scripts/native/bootstrap-deps.py",
    "scripts/native/deps-lock.json",
]

_QTBASE_STATIC_BUILD_RECORD = (
    "scripts/native/bootstrap-deps.py:build_qt() → cmake_project('qtbase', "
    "FEATURE_static=ON / FEATURE_shared=OFF / FEATURE_network=OFF / FEATURE_sql=OFF / "
    "FEATURE_dbus=OFF / FEATURE_opengl=OFF / FEATURE_system_*=OFF（bundled）/ "
    "FEATURE_static_runtime=ON(Win)，源码 = deps-lock.json sources[qtbase] "
    "(sha256 56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80)"
)
_NONQT_STATIC_BUILD_RECORD = (
    "scripts/native/bootstrap-deps.py:build_core()/build_parsers() → cmake_project(…, "
    "BUILD_SHARED_LIBS=OFF / CMAKE_BUILD_TYPE=Release / CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded)，"
    "源码 = deps-lock.json 对应 sources[name]（sha256 锁定）"
)
_QTSVG_BUILD_RECORD = (
    "scripts/native/bootstrap-deps.py:build_qt() → cmake_project('qtsvg', "
    "QT_BUILD_EXAMPLES=OFF / QT_BUILD_TESTS=OFF / FEATURE_precompile_header=OFF)，"
    "源码 = deps-lock.json sources[qtsvg] "
    "(sha256 35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438)"
)
_QWEBP_BUILD_GAP_REASON = (
    "本地构建材料缺失：.snow/native-tools 无 qtimageformats 下载/源码树/CMakeCache/"
    "install_manifest/SPDX（docs/native/licenses/native-release-gaps-current.json "
    "dependencyEvidence.qtimageformats.missingAtCheckedPortalPaths）；仓库内仅有 "
    "bootstrap-deps.py:build_imageformats() 的 WebP-only 构建定义与 source-only 归档取证"
)

_ENTRY_POINT_LICENSES = [
    LICENSES + "qtbase/LICENSES/BSD-3-Clause.txt",
]


def _row(
    artifact: str,
    upstream: str,
    *,
    version: dict[str, Any] | None,
    license_id: str,
    license_texts: list[str],
    linkage: str,
    build_record: str,
    build_record_files: list[str],
    exe_share: str = "",
    binding_state: str = "recorded",
    binding_gap_reason: str = "",
    link_map_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "artifact": artifact,
        "group": upstream.split()[0],
        "upstream": upstream,
        "version": version,
        "licenseId": license_id,
        "licenseTexts": license_texts,
        "linkage": linkage,
        "buildRecord": build_record,
        "buildRecordFiles": build_record_files,
        "bindingState": binding_state,
        "bindingGapReason": binding_gap_reason,
        # Binary-level evidence. Left None for the embedded 40-row matrix: the
        # verifier merges it from scripts/native/library-binding-evidence.json
        # so the facts stay in a versionable data file instead of if-branches.
        "linkMapEvidence": link_map_evidence,
        "exeShare": exe_share,
    }


def _lock_version(source: str) -> dict[str, Any]:
    return {"kind": "lock", "source": source}


def _attribution_version(relative: str) -> dict[str, Any]:
    return {"kind": "attribution", "path": relative, "field": "Version"}


_STATIC = "静态库（COFF archive，随静态 Qt 链入 EXE）"
_PLUGIN = "静态插件库（COFF archive，qt_import_plugins 静态注册进 EXE）"
_MODULE = "静态 Qt 模块库（COFF archive，链入 EXE）"

_MODULE_LICENSE_ID = (
    "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only"
)


def build_matrix_rows() -> list[dict[str, Any]]:
    """The 40 installed library paths, in inventory order.

    Machine-readable mirror of the published third-party binding inventory.
    EXE shares quote the size analysis §4/§5 (baseline MAP,
    25,784,320 B EXE); empty string = only group-level data published.
    """
    rows: list[dict[str, Any]] = []
    qt_module = dict(
        version=_lock_version("qtbase"),
        license_id=_MODULE_LICENSE_ID,
        license_texts=_QT_MODULE_LICENSES,
        linkage=_MODULE,
        build_record=_QTBASE_STATIC_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
    )
    # --- qtbase modules (12) ---
    rows.append(_row("Qt6Core.lib", "qtbase 6.8.3（module Core）", exe_share="3.48 MB raw（模块+插件组 13,904,433 B / 55.8%，est. ZIP 6,590,701 B）", **qt_module))
    rows.append(_row("Qt6Gui.lib", "qtbase 6.8.3（module Gui）", exe_share="4.61 MB raw", **qt_module))
    rows.append(_row("Qt6Widgets.lib", "qtbase 6.8.3（module Widgets）", exe_share="4.29 MB raw", **qt_module))
    rows.append(_row("Qt6Xml.lib", "qtbase 6.8.3（module Xml）", **qt_module))
    rows.append(_row("Qt6Concurrent.lib", "qtbase 6.8.3（module Concurrent）", **qt_module))
    rows.append(_row("Qt6Test.lib", "qtbase 6.8.3（module Test，FEATURE_testlib=ON 供 host 测试）", **qt_module))
    rows.append(_row(
        "Qt6EntryPoint.lib",
        "qtbase 6.8.3（module EntryPoint，upstream SBOM 缺库 File 条目，BSD-3-Clause 补证）",
        version=_lock_version("qtbase"),
        license_id="BSD-3-Clause",
        license_texts=_ENTRY_POINT_LICENSES,
        linkage=_MODULE,
        build_record=_QTBASE_STATIC_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
    ))
    rows.append(_row("Qt6ExampleIcons.lib", "qtbase 6.8.3（module ExampleIconsPrivate）", **qt_module))
    rows.append(_row("Qt6DeviceDiscoverySupport.lib", "qtbase 6.8.3（module DeviceDiscoverySupportPrivate）", **qt_module))
    rows.append(_row("Qt6FbSupport.lib", "qtbase 6.8.3（module FbSupportPrivate）", **qt_module))
    # --- qtbase bundled third-party (8) ---
    rows.append(_row(
        "Qt6BundledFreetype.lib",
        "qtbase 6.8.3 → bundled freetype",
        version=_attribution_version(NOTICE_INPUT_PREFIX + "LICENSES/qtbase/src/3rdparty/freetype/qt_attribution.json"),
        license_id="FTL OR GPL-2.0-only",
        license_texts=[LICENSES + "qtbase/LICENSES/FTL.txt", LICENSES + "qtbase/src/3rdparty/freetype/LICENSE.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_freetype=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.60 MB raw",
    ))
    rows.append(_row(
        "Qt6BundledHarfbuzz.lib",
        "qtbase 6.8.3 → bundled harfbuzz-ng",
        version=None,
        license_id="MIT（历史 SBOM 观察的表达式；仓库内无版本/许可元数据文件）",
        license_texts=[LICENSES + "qtbase/LICENSES/MIT.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_harfbuzz=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.80 MB raw",
    ))
    rows.append(_row(
        "Qt6BundledLibjpeg.lib",
        "qtbase 6.8.3 → bundled libjpeg",
        version=None,
        license_id="IJG AND BSD-3-Clause（历史 SBOM 观察的表达式）",
        license_texts=[LICENSES + "qtbase/LICENSES/IJG.txt", LICENSES + "qtbase/LICENSES/BSD-3-Clause.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_jpeg=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.51 MB raw",
    ))
    rows.append(_row(
        "Qt6BundledLibjpeg12bits.lib",
        "qtbase 6.8.3 → bundled libjpeg（12-bit 变体）",
        version=None,
        license_id="IJG AND BSD-3-Clause（历史 SBOM 观察的表达式）",
        license_texts=[LICENSES + "qtbase/LICENSES/IJG.txt", LICENSES + "qtbase/LICENSES/BSD-3-Clause.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_jpeg=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
    ))
    rows.append(_row(
        "Qt6BundledLibjpeg16bits.lib",
        "qtbase 6.8.3 → bundled libjpeg（16-bit 变体）",
        version=None,
        license_id="IJG AND BSD-3-Clause（历史 SBOM 观察的表达式）",
        license_texts=[LICENSES + "qtbase/LICENSES/IJG.txt", LICENSES + "qtbase/LICENSES/BSD-3-Clause.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_jpeg=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
    ))
    rows.append(_row(
        "Qt6BundledLibpng.lib",
        "qtbase 6.8.3 → bundled libpng",
        version=None,
        license_id="Libpng AND libpng-2.0（历史 SBOM 观察的表达式）",
        license_texts=[LICENSES + "qtbase/LICENSES/Libpng.txt", LICENSES + "qtbase/LICENSES/libpng-2.0.txt"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_png=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
    ))
    rows.append(_row(
        "Qt6BundledPcre2.lib",
        "qtbase 6.8.3 → bundled pcre2",
        version=_attribution_version(NOTICE_INPUT_PREFIX + "LICENSES/qtbase/src/3rdparty/pcre2/qt_attribution.json"),
        license_id="LicenseRef-BSD-3-Clause-with-PCRE2-Binary-Like-Packages-Exception",
        license_texts=[
            LICENSES + "qtbase/LICENSES/LicenseRef-BSD-3-Clause-with-PCRE2-Binary-Like-Packages-Exception.txt",
            LICENSES + "qtbase/src/3rdparty/pcre2/LICENCE.md",
        ],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_pcre2=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.51 MB raw",
    ))
    rows.append(_row(
        "Qt6BundledZLIB.lib",
        "qtbase 6.8.3 → bundled zlib",
        version=_attribution_version(NOTICE_INPUT_PREFIX + "LICENSES/qtbase/src/3rdparty/zlib/qt_attribution.json"),
        license_id="Zlib",
        license_texts=[LICENSES + "qtbase/LICENSES/Zlib.txt", LICENSES + "qtbase/src/3rdparty/zlib/LICENSE"],
        linkage=_STATIC,
        build_record=_QTBASE_STATIC_BUILD_RECORD + "，FEATURE_system_zlib=OFF → bundled",
        build_record_files=_QTBASE_BUILD_FILES,
    ))
    # --- qtbase plugins (8) ---
    plugin = dict(
        version=_lock_version("qtbase"),
        license_id=_MODULE_LICENSE_ID,
        license_texts=_QT_MODULE_LICENSES,
        linkage=_PLUGIN,
        build_record=_QTBASE_STATIC_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
    )
    rows.append(_row(
        "qwindows.lib",
        "qtbase 6.8.3（platform 插件 QWindowsIntegrationPlugin）",
        exe_share="0.77 MB raw",
        **plugin,
    ))
    rows.append(_row("qdirect2d.lib", "qtbase 6.8.3（platform 插件 QWindowsDirect2DIntegrationPlugin）", **plugin))
    rows.append(_row("qminimal.lib", "qtbase 6.8.3（platform 插件 QMinimalIntegrationPlugin）", **plugin))
    rows.append(_row("qoffscreen.lib", "qtbase 6.8.3（platform 插件 QOffscreenIntegrationPlugin）", **plugin))
    rows.append(_row(
        "qgif.lib",
        "qtbase 6.8.3（imageformat 插件 QGifPlugin）",
        exe_share="0.015 MB raw（14,716 B，§5 剪枝候选实测）",
        **plugin,
    ))
    rows.append(_row(
        "qico.lib",
        "qtbase 6.8.3（imageformat 插件 QICOPlugin）",
        exe_share="0.012 MB raw（12,132 B，§5）",
        **plugin,
    ))
    rows.append(_row(
        "qjpeg.lib",
        "qtbase 6.8.3（imageformat 插件 QJpegPlugin）",
        exe_share="0.019 MB raw（18,520 B，§5）",
        **plugin,
    ))
    rows.append(_row(
        "qmodernwindowsstyle.lib",
        "qtbase 6.8.3（style 插件 QModernWindowsStylePlugin）",
        **plugin,
    ))
    # --- qtsvg (4) ---
    qtsvg = dict(
        version=_lock_version("qtsvg"),
        license_id=_MODULE_LICENSE_ID,
        license_texts=_QTSVG_MODULE_LICENSES,
        linkage=_MODULE,
        build_record=_QTSVG_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
    )
    rows.append(_row("Qt6Svg.lib", "qtsvg 6.8.3（module Svg）", exe_share="0.48 MB raw", **qtsvg))
    rows.append(_row("Qt6SvgWidgets.lib", "qtsvg 6.8.3（module SvgWidgets）", **qtsvg))
    rows.append(_row(
        "qsvgicon.lib",
        "qtsvg 6.8.3（iconengine 插件 QSvgIconPlugin）",
        version=_lock_version("qtsvg"),
        license_id=_MODULE_LICENSE_ID,
        license_texts=_QTSVG_MODULE_LICENSES,
        linkage=_PLUGIN,
        build_record=_QTSVG_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.041 MB raw（40,720 B，§5）",
    ))
    rows.append(_row(
        "qsvg.lib",
        "qtsvg 6.8.3（imageformat 插件 QSvgPlugin）",
        version=_lock_version("qtsvg"),
        license_id=_MODULE_LICENSE_ID,
        license_texts=_QTSVG_MODULE_LICENSES,
        linkage=_PLUGIN,
        build_record=_QTSVG_BUILD_RECORD,
        build_record_files=_QTBASE_BUILD_FILES,
        exe_share="0.005 MB raw（5,300 B，§5）",
    ))
    # --- qtimageformats / libwebp (1) ---
    rows.append(_row(
        "qwebp.lib",
        "qtimageformats 6.8.3（imageformat 插件 QWebpPlugin）→ bundled libwebp 1.5.0",
        version=_lock_version("qtimageformats"),
        license_id=(
            "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only（qtimageformats）"
            " + libwebp BSD-3-Clause（bundled，独立第三方组件）"
        ),
        license_texts=_QTIMAGEFORMATS_MODULE_LICENSES + _LIBWEBP_LICENSES,
        linkage=_PLUGIN,
        build_record=(
            "scripts/native/bootstrap-deps.py:build_imageformats()（INPUT_webp=qt / "
            "FEATURE_webp=ON / FEATURE_system_webp=OFF → bundled libwebp / tiff+mng+jasper=OFF，"
            "targets QWebpPlugin+QWebpPlugin_init），源码 = deps-lock.json sources[qtimageformats] "
            "(sha256 049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8)"
        ),
        build_record_files=_QTBASE_BUILD_FILES + ["docs/native/licenses/qtimageformats-source-only-evidence.json"],
        binding_state="gap",
        binding_gap_reason=_QWEBP_BUILD_GAP_REASON,
        exe_share="0.51 MB raw（514,270 B，§5）",
    ))
    # --- non-Qt static libraries (9) ---
    rows.append(_row(
        "libcurl.lib",
        "curl 8.23.0",
        version=_lock_version("curl"),
        license_id="curl",
        license_texts=[LICENSES + "curl/COPYING"],
        linkage=_STATIC,
        build_record=_NONQT_STATIC_BUILD_RECORD.replace("…, ", "") + "；curl: BUILD_CURL_EXE=OFF / CURL_USE_SCHANNEL=ON(Win) / CURL_USE_OPENSSL=OFF(Win) / HTTP_ONLY=ON / CURL_DISABLE_LDAP=ON / CURL_STATIC_CRT=ON(Win)",
        build_record_files=_DEP_BUILD_FILES,
        exe_share="0.66 MB raw（非 Qt 静态组 2,136,600 B / 8.6%）",
    ))
    rows.append(_row(
        "archive.lib",
        "libarchive 3.8.9",
        version=_lock_version("libarchive"),
        license_id="BSD-2-Clause（根 COPYING；许可边界注明 BSD-3-Clause/CC0/Apache 混合覆盖未逐文件展开）",
        license_texts=[LICENSES + "libarchive/COPYING"],
        linkage=_STATIC,
        build_record=_NONQT_STATIC_BUILD_RECORD + "；libarchive: ENABLE_BZip2/LZMA/ZSTD=ON（指向 bootstrap 产物）/ ENABLE_CNG=ON(Win) / ENABLE_OPENSSL=OFF / ENABLE_LIBXML2=OFF",
        build_record_files=_DEP_BUILD_FILES,
    ))
    rows.append(_row(
        "zlib.lib",
        "zlib 1.3.1（导入库；zlib.dll 仅存在于安装树，非生产 EXE 形态）",
        version=_lock_version("zlib"),
        license_id="Zlib",
        license_texts=[LICENSES + "zlib/LICENSE"],
        linkage="导入库（import library，86 个 short-import 对象；生产 EXE 使用 zlibstatic.lib）",
        build_record=_NONQT_STATIC_BUILD_RECORD + "；zlib: ZLIB_BUILD_EXAMPLES=OFF（同一源码同时产出 zlibstatic.lib 与 zlib.dll+zlib.lib）",
        build_record_files=_DEP_BUILD_FILES,
    ))
    rows.append(_row(
        "zlibstatic.lib",
        "zlib 1.3.1",
        version=_lock_version("zlib"),
        license_id="Zlib",
        license_texts=[LICENSES + "zlib/LICENSE"],
        linkage=_STATIC,
        build_record=_NONQT_STATIC_BUILD_RECORD + "；zlib: ZLIB_BUILD_EXAMPLES=OFF",
        build_record_files=_DEP_BUILD_FILES,
    ))
    rows.append(_row(
        "lzma.lib",
        "xz（liblzma）5.8.1",
        version=_lock_version("xz"),
        license_id="0BSD（liblzma；归档内工具/构建文件另有 GPL/LGPL，见 COPYING）",
        license_texts=[LICENSES + "xz/COPYING.0BSD", LICENSES + "xz/COPYING"],
        linkage=_STATIC,
        build_record=_NONQT_STATIC_BUILD_RECORD + "；xz: BUILD_TESTING=OFF / XZ_TOOL_*=OFF / XZ_NLS=OFF",
        build_record_files=_DEP_BUILD_FILES,
    ))
    rows.append(_row(
        "zstd_static.lib",
        "zstd 1.5.7",
        version=_lock_version("zstd"),
        license_id="BSD-3-Clause OR GPL-2.0-only",
        license_texts=[LICENSES + "zstd/LICENSE", LICENSES + "zstd/COPYING"],
        linkage=_STATIC,
        build_record=_NONQT_STATIC_BUILD_RECORD + "；zstd: ZSTD_BUILD_SHARED=OFF / ZSTD_BUILD_STATIC=ON / ZSTD_BUILD_PROGRAMS=OFF / ZSTD_USE_STATIC_RUNTIME=ON",
        build_record_files=_DEP_BUILD_FILES,
        exe_share="0.51 MB raw",
    ))
    rows.append(_row(
        "bz2.lib",
        "bzip2 1.0.8",
        version=_lock_version("bzip2"),
        license_id="bzip2-1.0.6",
        license_texts=[LICENSES + "bzip2/LICENSE"],
        linkage=_STATIC,
        build_record=(
            "scripts/native/bootstrap-deps.py:build_core() → cmake_project('bzip2', "
            "source_override=scripts/native/deps/bzip2, BZIP2_SOURCE_DIR=<锁定的 bzip2-1.0.8 源码>)；"
            "包装器 scripts/native/deps/bzip2/CMakeLists.txt 显式列出 7 个 .c 源文件"
        ),
        build_record_files=_DEP_BUILD_FILES + ["scripts/native/deps/bzip2/CMakeLists.txt"],
    ))
    rows.append(_row(
        "gumbo.lib",
        "gumbo-parser 0.10.1",
        version=_lock_version("gumbo"),
        license_id="Apache-2.0",
        license_texts=[LICENSES + "gumbo/COPYING"],
        linkage=_STATIC,
        build_record=(
            "scripts/native/bootstrap-deps.py:build_parsers() → cmake_project('gumbo', "
            "source_override=scripts/native/deps/gumbo, GUMBO_SOURCE_DIR=<锁定的 gumbo-parser-0.10.1 源码>)；"
            "包装器 scripts/native/deps/gumbo/CMakeLists.txt 显式列出 11 个 .c 源文件"
        ),
        build_record_files=_DEP_BUILD_FILES + ["scripts/native/deps/gumbo/CMakeLists.txt"],
    ))
    rows.append(_row(
        "yaml.lib",
        "libyaml 0.2.5",
        version=_lock_version("yaml"),
        license_id="MIT",
        license_texts=[LICENSES + "yaml/License"],
        linkage=_STATIC,
        build_record=(
            "scripts/native/bootstrap-deps.py:build_parsers() → cmake_project('yaml-native', "
            "source_override=scripts/native/deps/yaml, LIBYAML_SOURCE_DIR=<锁定的 yaml-0.2.5 源码>)；"
            "包装器 scripts/native/deps/yaml/CMakeLists.txt（YAML_DECLARE_STATIC，显式 8 个 .c）"
        ),
        build_record_files=_DEP_BUILD_FILES + ["scripts/native/deps/yaml/CMakeLists.txt"],
    ))
    return rows


def header_only_rows() -> list[dict[str, Any]]:
    """Header-only consumers that are NOT part of the 40 installed .lib paths."""
    return [
        _row(
            "nlohmann/json（头文件库，不计入 40 个 .lib）",
            "nlohmann/json 3.11.3",
            version=_lock_version("json"),
            license_id="MIT",
            license_texts=[LICENSES + "json/LICENSE.MIT"],
            linkage="仅头文件（bootstrap-deps.py:build_core() JSON_MultipleHeaders=ON 安装 include/nlohmann）",
            build_record=_NONQT_STATIC_BUILD_RECORD + "；json: JSON_BuildTests=OFF / JSON_MultipleHeaders=ON",
            build_record_files=_DEP_BUILD_FILES,
        ),
        _row(
            "toml++（头文件库，不计入 40 个 .lib）",
            "tomlplusplus 3.4.0",
            version=_lock_version("tomlplusplus"),
            license_id="MIT",
            license_texts=[LICENSES + "tomlplusplus/LICENSE"],
            linkage="仅头文件（bootstrap-deps.py:build_parsers() 安装 include/toml++）",
            build_record=_NONQT_STATIC_BUILD_RECORD + "；tomlplusplus: BUILD_EXAMPLES=OFF",
            build_record_files=_DEP_BUILD_FILES,
        ),
    ]


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _lock_sources(lock: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {entry["name"]: entry for entry in lock.get("sources", [])}


def _evidence_file_path(repo_root: Path, evidence_file: Path | None) -> Path:
    return Path(evidence_file) if evidence_file is not None else (repo_root / EVIDENCE_PATH)


def _load_evidence(evidence_file: Path) -> dict[str, Any] | None:
    """Load the versioned binary-evidence data file; None when it is absent."""
    if not evidence_file.is_file():
        return None
    data = _load_json(evidence_file)
    if not isinstance(data, dict):
        raise ValueError("evidence file must contain a JSON object")
    if data.get("kind") != "library-binding-evidence":
        raise ValueError("evidence file kind must be 'library-binding-evidence'")
    return data


def _row_link_evidence(row: dict[str, Any], evidence: dict[str, Any] | None) -> dict[str, Any] | None:
    """Return the row's binary link evidence, merging the shared data file.

    A row may carry ``linkMapEvidence`` inline (used by fixtures and custom
    ``--matrix-file`` inputs); otherwise the artifact is looked up in the
    versionable data file and the shared ``linkMap`` provenance is merged in.
    """
    inline = row.get("linkMapEvidence")
    if isinstance(inline, dict):
        return inline
    if not isinstance(evidence, dict):
        return None
    entry = (evidence.get("rows") or {}).get(row.get("artifact"))
    if not isinstance(entry, dict):
        return None
    merged = dict(entry)
    link_map = evidence.get("linkMap") or {}
    merged.setdefault("mapPath", link_map.get("mapPath", ""))
    merged.setdefault("sourceReport", evidence.get("sourceReport", ""))
    return merged


def _parse_spdx_tag_value(path: Path) -> dict[str, dict[str, Any]]:
    """Parse an SPDX tag-value document into {PackageName: metadata}."""
    packages: dict[str, dict[str, Any]] = {}
    current: str | None = None
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith("PackageName:"):
            current = line.split(":", 1)[1].strip()
            entry = packages.setdefault(current, {"packageNameLine": number})
            entry.setdefault("packageName", current)
        elif current is not None and line.startswith("PackageVersion:"):
            version = line.split(":", 1)[1].strip()
            if version:
                packages[current].setdefault("version", version)
                packages[current].setdefault("versionLine", number)
        elif current is not None and line.startswith("PackageLicenseConcluded:"):
            packages[current].setdefault("licenseConcluded", line.split(":", 1)[1].strip())
    return packages


def _load_sbom(path: Path, version_map: dict[str, str]) -> dict[str, Any]:
    """Resolve repository-external SPDX evidence for the rows that need it."""
    packages = _parse_spdx_tag_value(path)
    resolved: dict[str, str] = {}
    missing: dict[str, str] = {}
    for artifact, package in version_map.items():
        entry = packages.get(package) or {}
        version = entry.get("version")
        if isinstance(version, str) and version.strip():
            resolved[artifact] = version.strip()
        else:
            missing[artifact] = package
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "packagesParsed": len(packages),
        "resolved": resolved,
        "missing": missing,
    }


def _check_version(
    row: dict[str, Any],
    lock: dict[str, Any],
    repo_root: Path,
    sbom: dict[str, Any] | None = None,
    sbom_version_map: dict[str, str] | None = None,
) -> tuple[str | None, str | None]:
    """Return (found_version, problem).

    Repository-external version evidence (the Qt install-tree SPDX SBOM) is
    never hard-coded: it is only consulted when ``--sbom <path>`` is supplied,
    and rows still report a gap when no SBOM is available.
    """
    version = row.get("version")
    if not isinstance(version, dict) or "kind" not in version:
        artifact = row.get("artifact", "")
        package = (sbom_version_map or {}).get(artifact)
        if package:
            if sbom is None:
                return None, (
                    "矩阵行缺少 version 证据（未记录，缺口）；"
                    f"该行版本只能由仓库外 SPDX SBOM 提供（package {package!r}），"
                    "可用 --sbom <path> 读取"
                )
            resolved = sbom.get("resolved", {}).get(artifact)
            if resolved:
                return resolved, None
            return None, (
                f"SBOM {sbom.get('path')} 中未找到 package {package!r} 的非空 "
                "PackageVersion（未记录，缺口）"
            )
        return None, "矩阵行缺少 version 证据（未记录，缺口）"
    kind = version["kind"]
    if kind == "lock":
        source = _lock_sources(lock).get(version.get("source", ""))
        if source is None:
            return None, f"deps-lock.json 中不存在源 {version.get('source')!r}"
        value = source.get("version")
        if not isinstance(value, str) or not value.strip():
            return None, f"deps-lock.json sources[{version.get('source')!r}].version 为空"
        return value, None
    if kind == "attribution":
        path = repo_root / version.get("path", "")
        if not path.is_file():
            return None, f"版本证据文件缺失：{version.get('path')}"
        try:
            data = _load_json(path)
        except (OSError, ValueError) as error:
            return None, f"版本证据文件不可解析：{version.get('path')}（{error}）"
        rows = data if isinstance(data, list) else [data]
        field = version.get("field", "Version")
        for entry in rows:
            if isinstance(entry, dict) and isinstance(entry.get(field), str) and entry[field].strip():
                return entry[field], None
        return None, f"版本证据文件 {version.get('path')} 中无非空字段 {field}"
    return None, f"未知 version 证据类型：{kind!r}"


def _check_license_texts(row: dict[str, Any], manifest_paths: set[str] | None, repo_root: Path) -> list[str]:
    problems: list[str] = []
    texts = row.get("licenseTexts")
    if not isinstance(texts, list) or not texts:
        return ["矩阵行未引用任何许可证文本（缺口）"]
    for relative in texts:
        path = repo_root / relative
        if not path.is_file():
            problems.append(f"许可证文本缺失：{relative}")
        elif path.stat().st_size == 0:
            problems.append(f"许可证文本为空文件：{relative}")
        elif manifest_paths is not None and relative not in manifest_paths:
            problems.append(f"许可证文本未被受信任 notice 清单覆盖：{relative}")
    if manifest_paths is None:
        problems.append(f"受信任 notice 清单不可读：{NOTICE_MANIFEST_PATH}")
    return problems


def _check_link_evidence(
    row: dict[str, Any], repo_root: Path, evidence: dict[str, Any] | None
) -> list[str]:
    """Validate the binary-level evidence grade for one matrix row.

    Fail-closed: anything that is neither a fully cited ``link-map-proved`` row
    nor a reason-bearing ``justified-absent`` row is a real binding gap.
    """
    entry = _row_link_evidence(row, evidence)
    if entry is None:
        return [_MISSING_LINK_EVIDENCE_REASON]
    problems: list[str] = []
    grade = entry.get("grade")
    if grade not in _GRADES:
        return [f"二进制链接证据等级非法：{grade!r}（允许值 {list(_GRADES)}）"]
    for key in ("mapPath", "sourceReport", "sourceSection"):
        value = entry.get(key)
        if not isinstance(value, str) or not value.strip():
            problems.append(f"二进制链接证据缺少 {key}")
    source_report = entry.get("sourceReport")
    if isinstance(source_report, str) and source_report.strip():
        # The citation is descriptive provenance (link map path/section).  The
        # reports it used to point at are no longer published in the
        # repository, so a missing in-repo file is not a binding gap by itself.
        pass
    symbol_count = entry.get("symbolCount")
    contribution = entry.get("contributionBytes")
    if not isinstance(symbol_count, int) or isinstance(symbol_count, bool) or symbol_count < 0:
        problems.append("二进制链接证据 symbolCount 必须是非负整数")
        symbol_count = None
    if not isinstance(contribution, int) or isinstance(contribution, bool) or contribution < 0:
        problems.append("二进制链接证据 contributionBytes 必须是非负整数")
        contribution = None
    archive_sha = entry.get("archiveSha256")
    if archive_sha is not None and (
        not isinstance(archive_sha, str) or not _SHA256_RE.match(archive_sha)
    ):
        problems.append("二进制链接证据 archiveSha256 必须是 64 位小写十六进制")
    if grade == _GRADE_PROVED:
        if symbol_count is not None and symbol_count <= 0:
            problems.append("link-map-proved 要求 symbolCount>0（0 符号不能证明链入）")
        if contribution is not None and contribution <= 0:
            problems.append("link-map-proved 要求 contributionBytes>0")
    elif grade == _GRADE_JUSTIFIED_ABSENT:
        reason = entry.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            problems.append("justified-absent 但未记录不链入理由（未证明 → 缺口）")
        if symbol_count not in (None, 0):
            problems.append(
                f"justified-absent 只能用于零符号归因的行（实测 symbolCount={symbol_count}）"
            )
        if contribution not in (None, 0):
            problems.append(
                f"justified-absent 只能用于零字节归因的行（实测 contributionBytes={contribution}）"
            )
    else:
        problems.append("二进制链接证据等级为 gap（未证明 → 缺口）")
    return problems


def _check_binding(
    row: dict[str, Any], repo_root: Path, evidence: dict[str, Any] | None = None
) -> list[str]:
    problems: list[str] = []
    files = row.get("buildRecordFiles")
    if not isinstance(files, list) or not files:
        problems.append("矩阵行未引用任何构建记录文件（缺口）")
    else:
        for relative in files:
            if not (repo_root / relative).is_file():
                problems.append(f"构建记录文件缺失：{relative}")
    if not isinstance(row.get("buildRecord"), str) or not row["buildRecord"].strip():
        problems.append("矩阵行缺少构建记录说明（脚本+源码+选项）")
    if row.get("bindingState") == "gap":
        reason = row.get("bindingGapReason") or "构建绑定记录不完整（未说明原因）"
        problems.append(reason)
    problems.extend(_check_link_evidence(row, repo_root, evidence))
    return problems


def _check_evidence_integrity(
    evidence: dict[str, Any] | None,
    repo_root: Path,
    matrix_rows: list[dict[str, Any]],
) -> list[str]:
    """Cross-check the evidence data file against its own recorded provenance.

    This never re-derives the link map (it lives outside the repository); it
    only proves that the recorded counts/hashes are internally consistent.  A
    cited source report is cross-checked when it is present in the repository;
    the citation itself stays descriptive provenance.
    """
    if evidence is None:
        return []
    problems: list[str] = []
    rows = evidence.get("rows")
    if not isinstance(rows, dict):
        return ["证据数据文件缺少 rows 映射"]
    matrix_artifacts = {
        row.get("artifact") for row in matrix_rows if isinstance(row.get("artifact"), str)
    }
    orphaned = sorted(set(rows) - matrix_artifacts)
    if orphaned:
        problems.append(f"证据数据文件含矩阵之外的行：{orphaned}")
    uncovered = sorted(matrix_artifacts - set(rows))
    if uncovered:
        problems.append(f"证据数据文件未覆盖矩阵行：{uncovered}")

    link_map = evidence.get("linkMap")
    if not isinstance(link_map, dict):
        problems.append("证据数据文件缺少 linkMap 块")
        link_map = {}
    source_report = evidence.get("sourceReport")
    report_text: str | None = None
    if not isinstance(source_report, str) or not source_report.strip():
        problems.append("证据数据文件缺少 sourceReport")
    elif (repo_root / source_report).is_file():
        report_text = (repo_root / source_report).read_text(encoding="utf-8", errors="replace")
    for key, label in (("mapSha256", "link map SHA-256"), ("exeSha256", "EXE SHA-256")):
        value = link_map.get(key)
        if not isinstance(value, str) or not _SHA256_RE.match(value):
            problems.append(f"{label} 不是 64 位小写十六进制")
        elif report_text is not None and value not in report_text:
            problems.append(f"{label} {value} 未出现在源报告 {source_report}（引用不闭合）")

    counts = {grade: 0 for grade in _GRADES}
    total_contribution = 0
    for entry in rows.values():
        if not isinstance(entry, dict):
            problems.append("证据数据文件 rows 含非对象条目")
            continue
        grade = entry.get("grade")
        if grade not in counts:
            problems.append(f"证据数据文件 rows 含非法等级：{grade!r}")
            continue
        counts[grade] += 1
        if grade == _GRADE_PROVED:
            value = entry.get("contributionBytes")
            if isinstance(value, int) and not isinstance(value, bool):
                total_contribution += value

    expected = evidence.get("expected")
    if not isinstance(expected, dict):
        problems.append("证据数据文件缺少 expected 块")
        return problems
    for grade, recorded in expected.items():
        if grade == "totalContributionBytes":
            continue
        if isinstance(recorded, int) and counts.get(grade) != recorded:
            problems.append(
                f"证据数据文件 {grade} 行数 {counts.get(grade)} ≠ 登记值 {recorded}"
            )
    recorded_total = expected.get("totalContributionBytes")
    if isinstance(recorded_total, int):
        if total_contribution != recorded_total:
            problems.append(
                f"link-map-proved 贡献字节合计 {total_contribution} ≠ 登记值 {recorded_total}"
            )
        elif report_text is not None and f"{total_contribution:,}" not in report_text:
            problems.append(
                f"贡献字节合计 {total_contribution:,} 未出现在源报告 {source_report}（引用不闭合）"
            )
    return problems


def evaluate(
    matrix_rows: list[dict[str, Any]],
    repo_root: Path,
    *,
    sbom_path: Path | None = None,
    evidence_file: Path | None = None,
) -> dict[str, Any]:
    """Run all read-only checks and return the report dictionary."""
    repo_root = repo_root.resolve()
    lock_sha256 = hashlib.sha256((repo_root / LOCK_PATH).read_bytes()).hexdigest()
    lock = _load_json(repo_root / LOCK_PATH)
    try:
        manifest = _load_json(repo_root / NOTICE_MANIFEST_PATH)
        manifest_paths = {notice["path"] for notice in manifest.get("notices", [])}
        manifest_count = manifest.get("noticeCount", len(manifest.get("notices", [])))
    except (OSError, ValueError, KeyError, TypeError):
        manifest_paths = None
        manifest_count = None

    evidence_path = _evidence_file_path(repo_root, evidence_file)
    evidence_error: str | None = None
    try:
        evidence = _load_evidence(evidence_path)
    except (OSError, ValueError) as error:
        evidence = None
        evidence_error = str(error)
    evidence_sha256 = (
        hashlib.sha256(evidence_path.read_bytes()).hexdigest()
        if evidence_path.is_file()
        else None
    )

    sbom_version_map: dict[str, str] = {}
    if isinstance(evidence, dict):
        raw_map = evidence.get("sbomVersionMap")
        if isinstance(raw_map, dict):
            sbom_version_map = {
                str(key): str(value).strip()
                for key, value in raw_map.items()
                if isinstance(value, str) and value.strip()
            }
    sbom = _load_sbom(Path(sbom_path).resolve(), sbom_version_map) if sbom_path is not None else None

    missing_version: list[dict[str, str]] = []
    missing_license: list[dict[str, Any]] = []
    missing_binding: list[dict[str, Any]] = []
    resolved_versions: dict[str, str] = {}
    grade_counts: dict[str, int] = {grade: 0 for grade in _GRADES}
    grade_counts["unrecorded"] = 0

    for row in matrix_rows:
        artifact = row.get("artifact")
        if not isinstance(artifact, str) or not artifact.strip():
            missing_binding.append({"artifact": "<malformed-row>", "problems": ["矩阵行缺少 artifact 字段"]})
            missing_version.append({"artifact": "<malformed-row>", "reason": "矩阵行缺少 artifact 字段"})
            missing_license.append({"artifact": "<malformed-row>", "problems": ["矩阵行缺少 artifact 字段"]})
            grade_counts["unrecorded"] += 1
            continue
        for key in _REQUIRED_ROW_KEYS:
            if key not in row:
                missing_binding.append({"artifact": artifact, "problems": [f"矩阵行缺少必需字段 {key}"]})
                break
        entry = _row_link_evidence(row, evidence)
        grade = entry.get("grade") if isinstance(entry, dict) else None
        if grade in grade_counts:
            grade_counts[grade] += 1
        else:
            grade_counts["unrecorded"] += 1
        found, problem = _check_version(row, lock, repo_root, sbom, sbom_version_map)
        if problem:
            missing_version.append({"artifact": artifact, "reason": problem})
        elif found:
            resolved_versions[artifact] = found
        license_problems = _check_license_texts(row, manifest_paths, repo_root)
        if license_problems:
            missing_license.append({"artifact": artifact, "problems": license_problems})
        binding_problems = _check_binding(row, repo_root, evidence)
        if binding_problems:
            missing_binding.append({"artifact": artifact, "problems": binding_problems})

    integrity_problems = _check_evidence_integrity(evidence, repo_root, matrix_rows)
    integrity_items = [{"artifact": "<evidence-data-file>", "problem": p} for p in integrity_problems]

    return {
        "schema": 1,
        "kind": "library-binding-verify",
        "readOnly": True,
        "lock": {"path": LOCK_PATH, "sha256": lock_sha256, "sources": len(lock.get("sources", []))},
        "noticeManifest": {"path": NOTICE_MANIFEST_PATH, "notices": manifest_count},
        "rows": len(matrix_rows),
        "evidence": {
            "path": (
                evidence_path.resolve().relative_to(repo_root).as_posix()
                if evidence_path.resolve().is_relative_to(repo_root)
                else str(evidence_path)
            ),
            "sha256": evidence_sha256,
            "loaded": evidence is not None,
            "error": evidence_error,
            "sourceReport": (evidence or {}).get("sourceReport"),
            "sourceCommit": (evidence or {}).get("sourceCommit"),
            "grades": grade_counts,
        },
        "evidenceIntegrity": {
            "ok": not integrity_problems,
            "problems": integrity_problems,
        },
        "gateFieldOwners": _GATE_FIELD_OWNERS,
        "sbom": {
            "provided": sbom is not None,
            "path": sbom["path"] if sbom else None,
            "sha256": sbom["sha256"] if sbom else None,
            "packagesParsed": sbom["packagesParsed"] if sbom else 0,
            "resolvedVersions": sbom["resolved"] if sbom else {},
            "missingPackages": sbom["missing"] if sbom else {},
        },
        "gaps": {
            "missingVersion": {"count": len(missing_version), "items": missing_version},
            "missingLicenseText": {"count": len(missing_license), "items": missing_license},
            "missingBinding": {"count": len(missing_binding), "items": missing_binding},
            "evidenceIntegrity": {"count": len(integrity_problems), "items": integrity_items},
        },
        "resolvedVersions": resolved_versions,
        "gapRowsTotal": (
            len(missing_version)
            + len(missing_license)
            + len(missing_binding)
            + len(integrity_problems)
        ),
    }


def _print_report(report: dict[str, Any]) -> None:
    print(f"library-binding-verify: {report['rows']} matrix rows (read-only)")
    print(f"lock: {report['lock']['path']} sha256={report['lock']['sha256']} sources={report['lock']['sources']}")
    print(f"notice manifest: {report['noticeManifest']['path']} notices={report['noticeManifest']['notices']}")
    evidence = report["evidence"]
    grade_text = ", ".join(f"{key}={value}" for key, value in evidence["grades"].items())
    state = "loaded" if evidence["loaded"] else "missing"
    print(
        f"evidence: {evidence['path']} sha256={evidence['sha256']} state={state} "
        f"source={evidence['sourceReport']}@{evidence['sourceCommit']} grades: {grade_text}"
    )
    if evidence["error"]:
        print(f"  ! evidence load error: {evidence['error']}")
    sbom = report["sbom"]
    if sbom["provided"]:
        print(
            f"sbom: {sbom['path']} sha256={sbom['sha256']} packages={sbom['packagesParsed']} "
            f"resolved={len(sbom['resolvedVersions'])}"
        )
    else:
        print("sbom: not provided (repository-external SPDX evidence not consulted; use --sbom <path>)")
    gaps = report["gaps"]
    for label, key in (
        ("missing-version", "missingVersion"),
        ("missing-license-text", "missingLicenseText"),
        ("missing-binding", "missingBinding"),
        ("evidence-integrity", "evidenceIntegrity"),
    ):
        entry = gaps[key]
        print(f"{label}: {entry['count']}")
        for item in entry["items"]:
            if "reason" in item:
                print(f"  - {item['artifact']}: {item['reason']}")
            elif "problems" in item:
                for problem in item["problems"]:
                    print(f"  - {item['artifact']}: {problem}")
            else:
                print(f"  - {item['artifact']}: {item['problem']}")
    if report["gapRowsTotal"] == 0:
        print("RESULT: no gaps")
    else:
        print(
            f"RESULT: {report['gapRowsTotal']} gap entries across "
            "version / license / binding / evidence-integrity"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT, help="repository root (default: script location)")
    parser.add_argument("--matrix-file", type=Path, help="optional JSON array overriding the embedded 40-row matrix")
    parser.add_argument(
        "--evidence-file",
        type=Path,
        help=f"optional JSON overriding the binary-evidence data file (default: {EVIDENCE_PATH})",
    )
    parser.add_argument(
        "--sbom",
        type=Path,
        help=(
            "SPDX tag-value SBOM from the Qt install tree; used only to resolve the rows "
            "listed in the evidence file's sbomVersionMap (never hard-coded)"
        ),
    )
    parser.add_argument("--json", action="store_true", help="machine-readable JSON output")
    parser.add_argument("--check", action="store_true", help="exit 1 when any gap exists")
    options = parser.parse_args(argv)

    repo_root = options.repo_root.resolve()
    if options.matrix_file is not None:
        try:
            rows = _load_json(options.matrix_file.resolve())
        except (OSError, ValueError) as error:
            print(f"cannot load matrix file {options.matrix_file}: {error}", file=sys.stderr)
            return 2
        if not isinstance(rows, list):
            print("matrix file must contain a JSON array of rows", file=sys.stderr)
            return 2
    else:
        rows = build_matrix_rows()

    sbom_path = options.sbom.resolve() if options.sbom is not None else None
    if sbom_path is not None and not sbom_path.is_file():
        print(f"cannot read SBOM file {sbom_path}", file=sys.stderr)
        return 2

    try:
        report = evaluate(
            rows,
            repo_root,
            sbom_path=sbom_path,
            evidence_file=options.evidence_file.resolve() if options.evidence_file else None,
        )
    except (OSError, ValueError) as error:
        print(f"cannot evaluate matrix: {error}", file=sys.stderr)
        return 2
    if options.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        _print_report(report)
    if options.check:
        return 1 if report["gapRowsTotal"] else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Host-side tests for the read-only library-binding matrix verifier.

Covers the contract points of scripts/native/library_binding_verify.py:
  * a complete fixture (build record + binary link evidence) exits 0 in
    --check mode, including when the only version evidence comes from
    ``--sbom <path>``;
  * constructed gaps exit non-zero and report the exact artifacts/reasons;
  * a ``justified-absent`` row with an empty reason is still a real gap;
  * the embedded 40-row matrix reports the expected real-repo gap counts
    (5 version / 0 license / 1 binding / 0 evidence-integrity) while the
    versioned data file encodes 27 ``link-map-proved`` + 13
    ``justified-absent`` rows;
  * ``--sbom`` resolves the five repository-external Qt SBOM versions only
    when explicitly supplied (never hard-coded).

Private fixtures only; the production trusted notice inputs and the evidence
data file are read but never modified.
Run with: python -B -m unittest scripts.native.tests.test_library_binding_verify
"""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
REPO_ROOT = HERE.parent.parent
SCRIPT = HERE / "library_binding_verify.py"
EVIDENCE_JSON = REPO_ROOT / "scripts" / "native" / "library-binding-evidence.json"
REAL_SBOM = Path(
    r"C:\Users\youtu\AppData\Local\nickel-native-tools\install\windows-x64-static\sbom\qtbase-6.8.3.spdx"
)

spec = importlib.util.spec_from_file_location("nickel_library_binding_verify", SCRIPT)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

EVIDENCE_REPORT = "evidence/fixture-linker.md"
FIXTURE_MAP_SHA = "1" * 64
FIXTURE_EXE_SHA = "2" * 64

SPDX_TEXT = (
    "SPDXVersion: SPDX-2.3\n"
    "PackageName: BundledAlpha\n"
    "PackageVersion: 9.9.9\n"
    "PackageLicenseConcluded: MIT\n"
    "\n"
    "PackageName: BundledBeta\n"
    "PackageVersion: 8.8.8\n"
    "PackageLicenseConcluded: BSD-3-Clause\n"
)


def _write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _link_evidence(**overrides) -> dict:
    evidence = {
        "grade": "link-map-proved",
        "mapPath": "build/fixture.map",
        "symbolCount": 42,
        "contributionBytes": 4096,
        "sourceReport": EVIDENCE_REPORT,
        "sourceSection": "§5 表 B fixture",
    }
    evidence.update(overrides)
    return evidence


def _complete_row(**overrides) -> dict:
    row = {
        "artifact": "testlib.lib",
        "group": "testlib",
        "upstream": "testlib 1.2.3",
        "version": {"kind": "lock", "source": "testlib"},
        "licenseId": "MIT",
        "licenseTexts": ["docs/licenses-inputs/LICENSE"],
        "linkage": "static",
        "buildRecord": "fake build script with recorded options",
        "buildRecordFiles": ["fake-build.py"],
        "bindingState": "recorded",
        "bindingGapReason": "",
        "linkMapEvidence": _link_evidence(),
        "exeShare": "",
    }
    row.update(overrides)
    return row


def _build_complete_repo(root: Path) -> None:
    """A minimal fake repository where every check passes."""
    manifest_dir = root / "docs" / "native" / "licenses"
    _write(
        root / "scripts" / "native" / "deps-lock.json",
        json.dumps({"schema": 1, "sources": [{"name": "testlib", "version": "1.2.3"}]}),
    )
    _write(
        manifest_dir / "trusted-launcher-notice-input.json",
        json.dumps({"noticeCount": 1, "notices": [{"path": "docs/licenses-inputs/LICENSE"}]}),
    )
    _write(root / "docs" / "licenses-inputs" / "LICENSE", "MIT text\n")
    _write(root / "fake-build.py", "print('build record')\n")
    _write(root / EVIDENCE_REPORT, "# fixture link map evidence\n")


class CompleteFixtureTests(unittest.TestCase):
    def test_complete_fixture_has_zero_gaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _build_complete_repo(root)
            report = verifier.evaluate([_complete_row()], root)
            self.assertEqual(report["gaps"]["missingVersion"]["count"], 0)
            self.assertEqual(report["gaps"]["missingLicenseText"]["count"], 0)
            self.assertEqual(report["gaps"]["missingBinding"]["count"], 0)
            self.assertEqual(report["gaps"]["evidenceIntegrity"]["count"], 0)
            self.assertEqual(report["gapRowsTotal"], 0)
            self.assertEqual(report["resolvedVersions"]["testlib.lib"], "1.2.3")
            self.assertEqual(report["evidence"]["grades"]["link-map-proved"], 1)

    def test_complete_fixture_cli_check_exits_zero(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _build_complete_repo(root)
            matrix = root / "matrix.json"
            matrix.write_text(json.dumps([_complete_row()]), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(root),
                 "--matrix-file", str(matrix)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("no gaps", completed.stdout)

    def test_attribution_version_evidence_resolves_field(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _build_complete_repo(root)
            _write(
                root / "scripts" / "native" / "deps-lock.json",
                json.dumps({"schema": 1, "sources": []}),
            )
            _write(
                root / "evidence" / "qt_attribution.json",
                json.dumps([{"Name": "Test", "Version": "9.9.9"}]),
            )
            row = _complete_row(
                version={"kind": "attribution", "path": "evidence/qt_attribution.json",
                         "field": "Version"})
            report = verifier.evaluate([row], root)
            self.assertEqual(report["gaps"]["missingVersion"]["count"], 0)
            self.assertEqual(report["resolvedVersions"]["testlib.lib"], "9.9.9")


class SbomVersionTests(unittest.TestCase):
    """``--sbom`` is the only way repository-external version evidence is used."""

    def _repo_with_evidence(self, root: Path, version_map: dict, rows: dict) -> Path:
        _build_complete_repo(root)
        _write(
            root / EVIDENCE_REPORT,
            "# fixture link map evidence\n"
            f"map sha256 {FIXTURE_MAP_SHA}\n"
            f"exe sha256 {FIXTURE_EXE_SHA}\n"
            "total 8,192 B\n"
            "row total 4,096 B\n",
        )
        expected = {
            "link-map-proved": sum(1 for item in rows.values()
                                   if item["grade"] == "link-map-proved"),
            "justified-absent": sum(1 for item in rows.values()
                                    if item["grade"] == "justified-absent"),
            "gap": 0,
            "totalContributionBytes": sum(
                item["contributionBytes"] for item in rows.values()
                if item["grade"] == "link-map-proved"),
        }
        evidence = {
            "schemaVersion": 1,
            "kind": "library-binding-evidence",
            "sourceReport": EVIDENCE_REPORT,
            "sourceCommit": "fixture",
            "linkMap": {"mapPath": "build/fixture.map", "mapSha256": FIXTURE_MAP_SHA,
                        "exeSha256": FIXTURE_EXE_SHA},
            "expected": expected,
            "sbomVersionMap": version_map,
            "rows": rows,
        }
        path = root / "fixture-evidence.json"
        path.write_text(json.dumps(evidence), encoding="utf-8")
        return path

    def test_sbom_supplies_versions_and_check_exits_zero(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            spdx = _write(root / "sbom" / "fixture.spdx", SPDX_TEXT)
            rows = {
                "alpha.lib": {"grade": "link-map-proved", "symbolCount": 10,
                              "contributionBytes": 4096, "sourceSection": "§5 fixture"},
                "beta.lib": {"grade": "link-map-proved", "symbolCount": 5,
                             "contributionBytes": 4096, "sourceSection": "§5 fixture"},
            }
            evidence_path = self._repo_with_evidence(
                root, {"alpha.lib": "BundledAlpha", "beta.lib": "BundledBeta"}, rows)
            matrix = [
                _complete_row(artifact="alpha.lib", version=None, linkMapEvidence=None),
                _complete_row(artifact="beta.lib", version=None, linkMapEvidence=None),
            ]
            matrix_path = root / "matrix.json"
            matrix_path.write_text(json.dumps(matrix), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(root),
                 "--matrix-file", str(matrix_path), "--evidence-file", str(evidence_path),
                 "--sbom", str(spdx)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("missing-version: 0", completed.stdout)
            self.assertIn("no gaps", completed.stdout)

    def test_without_sbom_the_same_rows_stay_version_gaps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rows = {"alpha.lib": {"grade": "link-map-proved", "symbolCount": 10,
                                  "contributionBytes": 4096, "sourceSection": "§5 fixture"}}
            evidence_path = self._repo_with_evidence(root, {"alpha.lib": "BundledAlpha"}, rows)
            matrix = [_complete_row(artifact="alpha.lib", version=None, linkMapEvidence=None)]
            matrix_path = root / "matrix.json"
            matrix_path.write_text(json.dumps(matrix), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(root),
                 "--matrix-file", str(matrix_path), "--evidence-file", str(evidence_path)],
                capture_output=True, text=True, timeout=120)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("missing-version: 1", completed.stdout)
            self.assertIn("--sbom <path>", completed.stdout)


class ConstructedGapTests(unittest.TestCase):
    def _repo(self, root: Path) -> None:
        _build_complete_repo(root)

    def test_each_gap_class_reports_exact_artifacts_and_reasons(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            rows = [
                _complete_row(artifact="no-version.lib", version=None),
                _complete_row(artifact="missing-text.lib",
                              licenseTexts=["docs/licenses-inputs/ABSENT.txt"]),
                _complete_row(artifact="empty-text.lib",
                              licenseTexts=["docs/licenses-inputs/EMPTY.txt"]),
                _complete_row(artifact="uncovered-text.lib",
                              licenseTexts=["docs/licenses-inputs/NOT-IN-MANIFEST.txt"]),
                _complete_row(artifact="no-binding.lib",
                              buildRecordFiles=["absent-build.py"]),
                _complete_row(artifact="gap-state.lib", bindingState="gap",
                              bindingGapReason="构造的绑定缺口"),
                _complete_row(artifact="no-evidence.lib", linkMapEvidence=None),
            ]
            _write(root / "docs" / "licenses-inputs" / "EMPTY.txt", "")
            _write(root / "docs" / "licenses-inputs" / "NOT-IN-MANIFEST.txt",
                   "text not in manifest\n")
            report = verifier.evaluate(rows, root)
            gaps = report["gaps"]
            self.assertEqual(gaps["missingVersion"]["count"], 1)
            self.assertEqual(gaps["missingVersion"]["items"][0]["artifact"], "no-version.lib")
            self.assertIn("version 证据", gaps["missingVersion"]["items"][0]["reason"])
            self.assertEqual(
                [item["artifact"] for item in gaps["missingLicenseText"]["items"]],
                ["missing-text.lib", "empty-text.lib", "uncovered-text.lib"])
            problems = {item["artifact"]: item["problems"]
                        for item in gaps["missingLicenseText"]["items"]}
            self.assertIn("许可证文本缺失：docs/licenses-inputs/ABSENT.txt",
                          problems["missing-text.lib"])
            self.assertIn("许可证文本为空文件：docs/licenses-inputs/EMPTY.txt",
                          problems["empty-text.lib"])
            self.assertIn("许可证文本未被受信任 notice 清单覆盖：docs/licenses-inputs/NOT-IN-MANIFEST.txt",
                          problems["uncovered-text.lib"])
            self.assertEqual(
                [item["artifact"] for item in gaps["missingBinding"]["items"]],
                ["no-binding.lib", "gap-state.lib", "no-evidence.lib"])
            binding = {item["artifact"]: item["problems"]
                       for item in gaps["missingBinding"]["items"]}
            self.assertIn("构建记录文件缺失：absent-build.py", binding["no-binding.lib"])
            self.assertIn("构造的绑定缺口", binding["gap-state.lib"])
            self.assertTrue(any("缺少二进制级链接证据" in problem
                                for problem in binding["no-evidence.lib"]))
            self.assertEqual(report["gapRowsTotal"], 7)

    def test_justified_absent_without_reason_is_still_a_gap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            row = _complete_row(
                artifact="silent-absent.lib",
                linkMapEvidence=_link_evidence(
                    grade="justified-absent", symbolCount=0, contributionBytes=0, reason="  "),
            )
            report = verifier.evaluate([row], root)
            items = report["gaps"]["missingBinding"]["items"]
            self.assertEqual([item["artifact"] for item in items], ["silent-absent.lib"])
            self.assertTrue(any("justified-absent 但未记录不链入理由" in problem
                                for problem in items[0]["problems"]))
            self.assertEqual(report["gapRowsTotal"], 1)

    def test_justified_absent_with_reason_is_not_a_gap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            row = _complete_row(
                artifact="proven-absent.lib",
                linkMapEvidence=_link_evidence(
                    grade="justified-absent", symbolCount=0, contributionBytes=0,
                    reason="未引用 Qt 模块；证据：link map §6a"),
            )
            report = verifier.evaluate([row], root)
            self.assertEqual(report["gaps"]["missingBinding"]["count"], 0)
            self.assertEqual(report["evidence"]["grades"]["justified-absent"], 1)
            self.assertEqual(report["gapRowsTotal"], 0)

    def test_proved_grade_with_zero_symbols_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            row = _complete_row(
                artifact="fake-proved.lib",
                linkMapEvidence=_link_evidence(symbolCount=0, contributionBytes=0),
            )
            report = verifier.evaluate([row], root)
            problems = report["gaps"]["missingBinding"]["items"][0]["problems"]
            self.assertTrue(any("symbolCount>0" in problem for problem in problems))
            self.assertTrue(any("contributionBytes>0" in problem for problem in problems))

    def test_evidence_missing_citation_field_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            row = _complete_row(
                artifact="dangling.lib",
                linkMapEvidence=_link_evidence(sourceReport=""),
            )
            report = verifier.evaluate([row], root)
            problems = report["gaps"]["missingBinding"]["items"][0]["problems"]
            self.assertTrue(any("缺少 sourceReport" in problem for problem in problems))

    def test_evidence_integrity_flags_tampered_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            _write(root / EVIDENCE_REPORT, "# fixture link map evidence without hashes\n")
            evidence = {
                "schemaVersion": 1,
                "kind": "library-binding-evidence",
                "sourceReport": EVIDENCE_REPORT,
                "sourceCommit": "fixture",
                "linkMap": {"mapPath": "build/fixture.map",
                            "mapSha256": FIXTURE_MAP_SHA, "exeSha256": FIXTURE_EXE_SHA},
                "expected": {"link-map-proved": 1, "justified-absent": 0, "gap": 0,
                             "totalContributionBytes": 4096},
                "rows": {"testlib.lib": {"grade": "link-map-proved", "symbolCount": 42,
                                         "contributionBytes": 4096, "sourceSection": "§5"}},
            }
            evidence_path = root / "tampered-evidence.json"
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            report = verifier.evaluate([_complete_row(linkMapEvidence=None)], root,
                                       evidence_file=evidence_path)
            self.assertGreater(report["gaps"]["evidenceIntegrity"]["count"], 0)
            problems = " ".join(item.get("problem", "")
                                for item in report["gaps"]["evidenceIntegrity"]["items"])
            self.assertIn("未出现在源报告", problems)

    def test_missing_version_license_and_binding_cli_check_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            rows = [_complete_row(artifact="no-version.lib", version=None)]
            matrix = root / "matrix.json"
            matrix.write_text(json.dumps(rows), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(root),
                 "--matrix-file", str(matrix)],
                capture_output=True, text=True, timeout=120)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("missing-version: 1", completed.stdout)
            self.assertIn("no-version.lib", completed.stdout)

    def test_malformed_rows_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            report = verifier.evaluate([{"artifact": "half.lib"}], root)
            self.assertGreater(report["gaps"]["missingLicenseText"]["count"], 0)
            binding_problems = report["gaps"]["missingBinding"]["items"][0]["problems"]
            self.assertTrue(any("缺少必需字段" in problem for problem in binding_problems))
            self.assertGreater(report["gapRowsTotal"], 0)

    def test_unreadable_notice_manifest_fails_closed_license_checks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._repo(root)
            (root / "docs" / "native" / "licenses"
             / "trusted-launcher-notice-input.json").write_text("{not json", encoding="utf-8")
            report = verifier.evaluate([_complete_row()], root)
            self.assertEqual(report["gaps"]["missingLicenseText"]["count"], 1)
            self.assertIn("受信任 notice 清单不可读",
                          report["gaps"]["missingLicenseText"]["items"][0]["problems"][0])


class EvidenceDataFileTests(unittest.TestCase):
    """Contract of scripts/native/library-binding-evidence.json."""

    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads(EVIDENCE_JSON.read_text(encoding="utf-8"))

    def test_grades_cover_all_40_rows_with_expected_split(self):
        rows = self.evidence["rows"]
        self.assertEqual(len(rows), 40)
        grades = [entry["grade"] for entry in rows.values()]
        self.assertEqual(grades.count("link-map-proved"), 27)
        self.assertEqual(grades.count("justified-absent"), 13)
        self.assertEqual(grades.count("gap"), 0)

    def test_contribution_bytes_match_published_total(self):
        total = sum(entry["contributionBytes"] for entry in self.evidence["rows"].values()
                    if entry["grade"] == "link-map-proved")
        self.assertEqual(total, 18875794)

    def test_justified_absent_reasons_cite_link_map_sections(self):
        for artifact, entry in self.evidence["rows"].items():
            if entry["grade"] != "justified-absent":
                continue
            reason = entry.get("reason", "")
            self.assertIn("link map", reason, artifact)
            self.assertIn(entry["sourceSection"], reason, artifact)
            self.assertEqual(entry["symbolCount"], 0, artifact)
            self.assertEqual(entry["contributionBytes"], 0, artifact)

    def test_justified_absent_classes_match_report_sections(self):
        by_section = {}
        for artifact, entry in self.evidence["rows"].items():
            if entry["grade"] == "justified-absent":
                by_section.setdefault(entry["sourceSection"], set()).add(artifact)
        self.assertEqual(
            by_section["§6a"],
            {"Qt6Xml.lib", "Qt6Concurrent.lib", "Qt6Test.lib", "Qt6ExampleIcons.lib",
             "Qt6DeviceDiscoverySupport.lib", "Qt6FbSupport.lib", "Qt6SvgWidgets.lib"})
        self.assertEqual(by_section["§6b"],
                         {"Qt6BundledLibjpeg12bits.lib", "Qt6BundledLibjpeg16bits.lib"})
        self.assertEqual(by_section["§6c"], {"zlib.lib"})
        self.assertEqual(by_section["§6d"],
                         {"qdirect2d.lib", "qminimal.lib", "qoffscreen.lib"})

    def test_artifact_set_matches_the_embedded_matrix(self):
        matrix = {row["artifact"] for row in verifier.build_matrix_rows()}
        self.assertEqual(matrix, set(self.evidence["rows"]))


class RealRepositoryMatrixTests(unittest.TestCase):
    """Drift detection against the actual repository fact sources."""

    @classmethod
    def setUpClass(cls):
        cls.report = verifier.evaluate(verifier.build_matrix_rows(), REPO_ROOT)

    def test_embedded_matrix_has_exactly_40_library_rows(self):
        self.assertEqual(len(verifier.build_matrix_rows()), 40)
        self.assertEqual(len({row["artifact"] for row in verifier.build_matrix_rows()}), 40)
        self.assertEqual(verifier.header_only_rows()[0]["upstream"], "nlohmann/json 3.11.3")

    def test_lock_and_notice_manifest_fingerprints(self):
        self.assertEqual(self.report["lock"]["sha256"],
                         "b974a9bcd5b9f6fab32ffbf248ccd23ab9938ff731a91c9aff2c162d70dd136d")
        self.assertEqual(self.report["lock"]["sources"], 13)
        self.assertEqual(self.report["noticeManifest"]["notices"], 143)

    def test_expected_gap_counts_after_evidence_wiring(self):
        gaps = self.report["gaps"]
        self.assertEqual(gaps["missingVersion"]["count"], 5)
        self.assertEqual(
            sorted(item["artifact"] for item in gaps["missingVersion"]["items"]),
            ["Qt6BundledHarfbuzz.lib", "Qt6BundledLibjpeg.lib", "Qt6BundledLibjpeg12bits.lib",
             "Qt6BundledLibjpeg16bits.lib", "Qt6BundledLibpng.lib"])
        self.assertEqual(gaps["missingLicenseText"]["count"], 0)
        # 40 -> 1: the 40 hard-coded exeClosure gaps are replaced by 27 proved
        # + 13 justified-absent rows; the single remaining binding gap is
        # qwebp's still-open local build-material gap (not an exeClosure item).
        self.assertEqual(gaps["missingBinding"]["count"], 1)
        self.assertEqual([item["artifact"] for item in gaps["missingBinding"]["items"]],
                         ["qwebp.lib"])
        self.assertEqual(gaps["evidenceIntegrity"]["count"], 0)
        self.assertEqual(self.report["gapRowsTotal"], 6)

    def test_evidence_grades_are_counted_for_the_real_matrix(self):
        grades = self.report["evidence"]["grades"]
        self.assertEqual(grades["link-map-proved"], 27)
        self.assertEqual(grades["justified-absent"], 13)
        self.assertEqual(grades["gap"], 0)
        self.assertEqual(grades["unrecorded"], 0)
        self.assertTrue(self.report["evidenceIntegrity"]["ok"])
        self.assertEqual(self.report["evidence"]["sourceCommit"], "5360620")

    def test_no_row_still_reports_the_obsolete_hardcoded_exe_closure_reason(self):
        for item in self.report["gaps"]["missingBinding"]["items"]:
            for problem in item["problems"]:
                self.assertNotIn("finalExecutableUseVerified", problem)
                self.assertNotIn("unverifiedGates", problem)
        self.assertNotIn("native-release-gaps-current.json unverifiedGates",
                         json.dumps(self.report["gaps"], ensure_ascii=False))
        # The correction itself stays machine-readable, quoting the old string.
        self.assertIn("native-release-gaps-current.json unverifiedGates",
                      self.report["gateFieldOwners"]["correctedFrom"])

    def test_gate_field_owners_point_at_the_real_files(self):
        owners = self.report["gateFieldOwners"]
        self.assertNotIn("docs/native/licenses/native-release-gaps-current.json",
                         owners["unverifiedGates"])
        self.assertIn("docs/native/licenses/native-dependency-evidence.json",
                      owners["unverifiedGates"])
        self.assertIn("docs/native/licenses/native-dependency-evidence.json",
                      owners["finalExecutableUseVerified"])
        self.assertIn("docs/native/licenses/native-release-gaps-current.json",
                      owners["nativeReleaseMissingMaterials"])

    def test_every_row_cites_only_existing_nonempty_manifest_covered_license_texts(self):
        for row in verifier.build_matrix_rows() + verifier.header_only_rows():
            for relative in row["licenseTexts"]:
                path = REPO_ROOT / relative
                self.assertTrue(path.is_file(), relative)
                self.assertGreater(path.stat().st_size, 0, relative)

    def test_qwebp_row_keeps_its_build_material_gap(self):
        row = next(item for item in verifier.build_matrix_rows()
                   if item["artifact"] == "qwebp.lib")
        self.assertEqual(row["bindingState"], "gap")
        report_row = next(item for item in self.report["gaps"]["missingBinding"]["items"]
                          if item["artifact"] == "qwebp.lib")
        self.assertTrue(any("本地构建材料缺失" in problem
                            for problem in report_row["problems"]))
        # It is still link-map-proved: the binary evidence is not the open item.
        evidence = json.loads(EVIDENCE_JSON.read_text(encoding="utf-8"))["rows"]["qwebp.lib"]
        self.assertEqual(evidence["grade"], "link-map-proved")
        self.assertGreater(evidence["contributionBytes"], 0)

    def test_cli_report_mode_exits_zero_even_with_gaps(self):
        completed = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--repo-root", str(REPO_ROOT)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("missing-version: 5", completed.stdout)
        self.assertIn("missing-license-text: 0", completed.stdout)
        self.assertIn("missing-binding: 1", completed.stdout)
        self.assertIn("evidence-integrity: 0", completed.stdout)
        self.assertIn("link-map-proved=27", completed.stdout)
        self.assertIn("justified-absent=13", completed.stdout)

    def test_check_mode_still_exits_nonzero_on_real_gaps(self):
        completed = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(REPO_ROOT)],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)

    def test_matrix_json_round_trip_keeps_counts(self):
        rows = verifier.build_matrix_rows()
        with tempfile.TemporaryDirectory() as temporary:
            matrix = Path(temporary) / "matrix.json"
            matrix.write_text(json.dumps(rows, ensure_ascii=False), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "--check", "--repo-root", str(REPO_ROOT),
                 "--matrix-file", str(matrix)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(completed.returncode, 1)
            self.assertIn("missing-version: 5", completed.stdout)
            self.assertIn("missing-license-text: 0", completed.stdout)
            self.assertIn("missing-binding: 1", completed.stdout)

    @unittest.skipUnless(REAL_SBOM.is_file(), "Qt install-tree SBOM is not present on this host")
    def test_real_sbom_resolves_the_five_external_versions(self):
        report = verifier.evaluate(verifier.build_matrix_rows(), REPO_ROOT, sbom_path=REAL_SBOM)
        self.assertEqual(report["gaps"]["missingVersion"]["count"], 0)
        self.assertEqual(report["sbom"]["sha256"],
                         "665a467098f536231c6e190d0085a826a2cf7049b52c406e190d07e6d0ba7e58")
        self.assertEqual(report["resolvedVersions"]["Qt6BundledHarfbuzz.lib"], "10.4.0")
        self.assertEqual(report["resolvedVersions"]["Qt6BundledLibjpeg.lib"], "3.1.0")
        self.assertEqual(report["resolvedVersions"]["Qt6BundledLibjpeg12bits.lib"], "3.1.0")
        self.assertEqual(report["resolvedVersions"]["Qt6BundledLibjpeg16bits.lib"], "3.1.0")
        self.assertEqual(report["resolvedVersions"]["Qt6BundledLibpng.lib"], "1.6.47")
        # qwebp's local build-material gap is not a version gap and stays open.
        self.assertEqual(report["gaps"]["missingBinding"]["count"], 1)

    def test_script_never_writes_inside_repository(self):
        """Evaluate twice and confirm tracked fact sources keep identical bytes."""
        watched = [
            REPO_ROOT / "scripts" / "native" / "deps-lock.json",
            REPO_ROOT / "scripts" / "native" / "library-binding-evidence.json",
            REPO_ROOT / "docs" / "native" / "licenses" / "trusted-launcher-notice-input.json",
        ]
        before = {path: path.read_bytes() for path in watched}
        verifier.evaluate(verifier.build_matrix_rows(), REPO_ROOT)
        for path, content in before.items():
            self.assertEqual(path.read_bytes(), content)


if __name__ == "__main__":
    unittest.main()

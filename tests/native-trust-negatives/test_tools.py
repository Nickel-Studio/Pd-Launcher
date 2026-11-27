"""Small orchestration regressions, never production/trust acceptance."""
from __future__ import annotations

import copy
import os
from pathlib import Path
import tempfile
import unittest

import evidence_tools as evidence


class EvidenceTests(unittest.TestCase):
    def passed(self, case_id: str) -> dict:
        return {"id": case_id, "status": "PASS", "executed": True, "assertions": 1,
                "evidence": {"actualAppServices": True, "includeLauncher": True,
                             "destinationExists": False, "productionReceiptUsed": False}}

    def document(self) -> dict:
        cases = []
        for case_id in evidence.inventory():
            if case_id.startswith("actual-") or case_id.endswith("-branch"):
                cases.append({"id": case_id, "status": "NOT_RUN", "executed": False, "assertions": 0,
                              "reason": "Synthetic orchestration fixture: actual product not run"})
            else:
                cases.append(self.passed(case_id))
        return {"schema": "pd.trust-negatives.final.v1", "scope": "production-appservices-negative-only",
                "productAccepted": False, "sourceCommit": "a" * 40, "sourceSha256": "b" * 64,
                "graphSha256": "c" * 64, "status": "NOT_RUN", "cases": cases}

    def graph(self) -> dict:
        return {"schema": "pd.trusted-export.graph.v1", "sourceCommit": "a" * 40,
                "sourceSha256": "b" * 64, "appServicesImported": False,
                "appServicesSources": ["AppServices.cpp", "application_state.cpp",
                                       "service/resource_actions.cpp", "service/trusted_launcher.cpp"]}

    def audit(self, document=None, graph=None):
        return evidence.audit_result(document or self.document(), graph or self.graph(), "c" * 64, "a" * 40, "b" * 64)

    def test_frozen_oracle_is_exact_original(self):
        self.assertEqual(evidence.tools.__file__, str(evidence.WORKTREE / evidence.ORACLE_PATH))
        self.assertEqual(evidence.tools.aggregate([], ["missing"])["status"], "NOT_RUN")

    def test_empty_inventory_is_not_run_77(self):
        result = evidence.final_inventory([])
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertEqual(evidence.exit_code(result), 77)
        self.assertFalse(result["productAccepted"])

    def test_partial_negatives_cannot_upgrade_product(self):
        result = self.audit()
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertFalse(result["productAccepted"])
        self.assertFalse(result["actualPdRoundTripPass"])
        self.assertFalse(result["productionLinkClosureIndependentlyProved"])

    def test_failure_dominates_not_run(self):
        document = self.document()
        document["cases"][0]["status"] = "FAIL"
        document["status"] = "FAIL"
        result = self.audit(document)
        self.assertEqual(evidence.exit_code(result), 1)
        self.assertGreater(result["counts"]["NOT_RUN"], 0)

    def test_late_hidden_failure_is_rejected(self):
        document = self.document()
        document["cases"][0]["status"] = "FAIL"
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(document)

    def test_unknown_or_duplicate_case_rejected(self):
        for case_id in ("primitive-mock-pass", evidence.inventory()[0]):
            document = self.document()
            document["cases"].append(self.passed(case_id))
            with self.assertRaises(evidence.tools.Rejected):
                self.audit(document)

    def test_unexecuted_pass_rejected(self):
        document = self.document()
        document["cases"][0]["executed"] = False
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(document)

    def test_not_run_cannot_have_executed_assertions(self):
        document = self.document()
        document["cases"][-1]["assertions"] = 1
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(document)

    def test_public_negative_cannot_disable_launcher(self):
        document = self.document()
        document["cases"][2]["evidence"]["includeLauncher"] = False
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(document)

    def test_public_negative_cannot_claim_mock_or_publication(self):
        for key, value in (("actualAppServices", False), ("productionReceiptUsed", True), ("destinationExists", True)):
            document = self.document()
            document["cases"][2]["evidence"][key] = value
            with self.assertRaises(evidence.tools.Rejected):
                self.audit(document)

    def test_generation_pin_mismatch_rejected(self):
        for key in ("sourceCommit", "sourceSha256", "graphSha256"):
            document = self.document()
            document[key] = "d" * len(document[key])
            with self.assertRaises(evidence.tools.Rejected):
                self.audit(document)

    def test_imported_or_missing_factory_graph_rejected(self):
        graph = self.graph()
        graph["appServicesImported"] = True
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(graph=graph)
        graph = self.graph()
        graph["appServicesSources"].pop()
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(graph=graph)

    def test_claimed_product_pass_rejected(self):
        document = self.document()
        document["productAccepted"] = True
        with self.assertRaises(evidence.tools.Rejected):
            self.audit(document)

    def test_isolation_preserves_compiler_environment(self):
        before = dict(os.environ)
        with tempfile.TemporaryDirectory(prefix=".tool-isolation-", dir=evidence.HERE) as path:
            result = evidence.isolated_environment(Path(path))
            for name in ("PATH", "INCLUDE", "LIB"):
                self.assertEqual(result.get(name), before.get(name))
            for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "TEMP", "TMP"):
                self.assertTrue(Path(result[name]).is_relative_to(Path(path)))
            self.assertNotIn("QT_PLUGIN_PATH", result)

    def test_owned_path_escape_rejected(self):
        with self.assertRaises(evidence.tools.Rejected):
            evidence.owned(evidence.HERE.parent / "native-production-smoke")

    def test_original_parser_rejects_duplicate_json(self):
        with self.assertRaises(evidence.tools.Rejected):
            evidence.tools.strict_json(b'{"status":"PASS","status":"FAIL"}')

    def test_snapshot_detects_same_size_mutation(self):
        with tempfile.TemporaryDirectory(prefix=".tool-file-", dir=evidence.HERE) as path:
            root = Path(path)
            target = root / "inert.txt"
            target.write_bytes(b"original")
            token = evidence.tools.file_snapshot(target, root, 1024)
            target.write_bytes(b"modified")
            with self.assertRaises(evidence.tools.Rejected):
                evidence.tools.unchanged(token, target, root)

    def test_all_pass_still_only_evidence_scope(self):
        cases = [self.passed(case_id) for case_id in evidence.inventory()]
        result = evidence.final_inventory(copy.deepcopy(cases))
        self.assertEqual(result["status"], "PASS")
        self.assertFalse(result["productAccepted"])
        self.assertEqual(result["scope"], "negative-evidence-audit-only")


if __name__ == "__main__":
    unittest.main(verbosity=2)

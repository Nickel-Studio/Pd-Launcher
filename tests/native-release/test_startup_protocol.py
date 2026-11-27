"""Synthetic regression for the strict startup measurement protocol.

No launcher is launched here: every input is generated data and every
assertion pins a pre-declared protocol rule, so a silent change of the
statistics (outlier rule, bootstrap, alternation order, L2 settle rule)
or of the machine-readable document shape fails the suite instead of
quietly changing a measurement.  See tests/native-release/startup_protocol.py.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import startup_protocol as sp  # noqa: E402


class MedianIqrMadTests(unittest.TestCase):
    def test_known_five_value_sample(self):
        stats = sp.median_iqr_mad([1, 2, 3, 4, 5])
        self.assertEqual(stats["count"], 5)
        self.assertEqual(stats["median"], 3)
        self.assertEqual(stats["q1"], 2)
        self.assertEqual(stats["q3"], 4)
        self.assertEqual(stats["iqr"], 2)
        self.assertEqual(stats["mad"], 1)
        self.assertAlmostEqual(stats["madSigma"], 1.4826)

    def test_empty_and_single_samples(self):
        self.assertIsNone(sp.median_iqr_mad([]))
        single = sp.median_iqr_mad([7.5])
        self.assertEqual(single["median"], 7.5)
        self.assertEqual(single["min"], 7.5)
        self.assertEqual(single["max"], 7.5)

    def test_quantile_inclusive_linear_interpolation(self):
        self.assertEqual(sp.quantile_inclusive([10, 20, 30, 40], 0.25), 17.5)
        self.assertEqual(sp.quantile_inclusive([10, 20, 30, 40], 0.75), 32.5)
        self.assertEqual(sp.quantile_inclusive([10], 0.5), 10)


class BootstrapCiTests(unittest.TestCase):
    VALUES = [600, 610, 590, 605, 595, 615, 585, 600, 620, 580, 610, 590, 598, 602, 604]

    def test_deterministic_under_fixed_seed(self):
        first = sp.bootstrap_ci_median(self.VALUES, samples=2000, seed=20261124)
        second = sp.bootstrap_ci_median(self.VALUES, samples=2000, seed=20261124)
        self.assertEqual(first, second)

    def test_ci_bounds_bracket_median_and_narrow_vs_range(self):
        ci = sp.bootstrap_ci_median(self.VALUES, samples=4000, seed=20261124)
        median = sp.median_iqr_mad(self.VALUES)["median"]
        self.assertLessEqual(ci[0], median)
        self.assertGreaterEqual(ci[1], median)
        self.assertLess(ci[1] - ci[0], max(self.VALUES) - min(self.VALUES))

    def test_edge_cases(self):
        self.assertIsNone(sp.bootstrap_ci_median([], samples=10, seed=1))
        self.assertEqual(sp.bootstrap_ci_median([42.0], samples=10, seed=1), [42.0, 42.0])


class OutlierRuleTests(unittest.TestCase):
    def test_modified_z_outlier_is_excluded_and_kept_set_restatistics(self):
        values = [600, 610, 590, 605, 595, 615, 585, 600, 620, 580, 610, 590, 598, 602, 1500]
        result = sp.outlier_rule(values)
        self.assertIn("iglewicz-hoban", result["rule"])
        self.assertEqual(len(result["kept"]), 14)
        self.assertEqual([entry["value"] for entry in result["excluded"]], [1500])
        self.assertEqual(result["excluded"][0]["index"], 14)
        self.assertIn("modifiedZ", result["excluded"][0]["reason"])
        self.assertEqual(sp.median_iqr_mad(result["kept"])["median"], 600)

    def test_tiny_groups_are_never_trimmed(self):
        result = sp.outlier_rule([1.0, 2.0, 999.0])
        self.assertEqual(result["rule"], "none(group<4)")
        self.assertEqual(result["kept"], [1.0, 2.0, 999.0])
        self.assertEqual(result["excluded"], [])

    def test_constant_group_trims_nothing(self):
        result = sp.outlier_rule([5.0] * 10)
        self.assertEqual(result["kept"], [5.0] * 10)
        self.assertEqual(result["excluded"], [])

    def test_mad_zero_fallback_uses_tukey_extreme_fences(self):
        # >50% of values sit on the median (MAD == 0) with a spread tail.
        values = [1.0, 1.0, 1.0, 1.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0,
                  5.0, 5.0, 5.0, 5.0, 9.0, 9.0, 9.0, 9.0, 5000.0]
        result = sp.outlier_rule(values)
        self.assertIn("tukey-extreme-fences", result["rule"])
        self.assertIn(5000.0, [entry["value"] for entry in result["excluded"]])

    def test_summarize_group_reports_exclusions_and_ci(self):
        values = [600, 610, 590, 605, 595, 615, 585, 600, 620, 580, 610, 590, 598, 602, 1500]
        group = sp.summarize_group(values, bootstrap=500, seed=20261124)
        self.assertEqual(group["nRaw"], 15)
        self.assertEqual(group["nKept"], 14)
        self.assertEqual(len(group["excluded"]), 1)
        self.assertIsNotNone(group["stats"])
        self.assertIsNotNone(group["ci95Median"])


class AlternationAndPairingTests(unittest.TestCase):
    def test_order_alternates_starter_and_keeps_one_slot_per_role(self):
        order = sp.build_ab_order(5, ["pd", "legacy"])
        self.assertEqual(len(order), 5)
        self.assertEqual(order[0], ["pd", "legacy"])
        self.assertEqual(order[1], ["legacy", "pd"])
        self.assertEqual(order[4], ["pd", "legacy"])
        for round_slots in order:
            self.assertEqual(sorted(round_slots), ["legacy", "pd"])

    def test_order_is_exactly_fifteen_rounds_for_protocol_default(self):
        order = sp.build_ab_order(15, ["pd", "legacy"])
        self.assertEqual(len(order), 15)
        starters = [slots[0] for slots in order]
        self.assertEqual(starters, ["pd", "legacy"] * 7 + ["pd"])

    def test_single_role_order_degenerates_gracefully(self):
        order = sp.build_ab_order(3, ["pd"])
        self.assertEqual(order, [["pd"], ["pd"], ["pd"]])

    def test_zero_rounds(self):
        self.assertEqual(sp.build_ab_order(0, ["pd", "legacy"]), [])

    def test_paired_differences_elementwise(self):
        self.assertEqual(sp.paired_differences([10.0, 20.0], [12.0, 17.0]), [-2.0, 3.0])
        with self.assertRaises(ValueError):
            sp.paired_differences([1.0], [1.0, 2.0])


class FrameRuleTests(unittest.TestCase):
    def test_uniform_frame_is_not_structured(self):
        self.assertFalse(sp.frame_is_structured(b"\x88" * 1000, 12))
        self.assertFalse(sp.frame_is_structured(b"\xff" * (128 * 80 * 4), 12))

    def test_content_like_frame_is_structured(self):
        self.assertTrue(sp.frame_is_structured(bytes(range(256)) * 4, 12))

    def test_threshold_is_inclusive(self):
        data = bytes(range(12))  # exactly 12 distinct values
        self.assertTrue(sp.frame_is_structured(data, 12))
        self.assertFalse(sp.frame_is_structured(bytes(range(11)), 12))

    def test_settle_index_qt_like_already_stable(self):
        hashes = ["a"] * 10
        structured = [True] * 10
        self.assertEqual(sp.settle_index(structured, hashes, 5), 4)

    def test_settle_index_legacy_like_blank_then_content(self):
        hashes = ["b"] * 7 + ["a"] * 8
        structured = [False] * 7 + [True] * 8
        self.assertEqual(sp.settle_index(structured, hashes, 5), 11)

    def test_settle_index_respects_first_index(self):
        hashes = ["a"] * 10
        structured = [True] * 10
        self.assertEqual(sp.settle_index(structured, hashes, 5, first_index=6), 6)

    def test_settle_index_never_settles_for_unstructured_stable_frames(self):
        self.assertIsNone(sp.settle_index([False] * 10, ["a"] * 10, 5))

    def test_settle_index_flapping_content_never_settles(self):
        hashes = ["a", "b"] * 10
        structured = [True] * 20
        self.assertIsNone(sp.settle_index(structured, hashes, 5))

    def test_settle_index_short_streams(self):
        self.assertIsNone(sp.settle_index([True] * 3, ["a"] * 3, 5))
        self.assertIsNone(sp.settle_index([True], ["a"], 5))


class AssembleStatsTests(unittest.TestCase):
    @staticmethod
    def sample(order, role, phase, round_no, l1, l2, failed=False, warmup=False):
        return {"orderIndex": order, "role": role, "phase": phase, "round": round_no,
                "slot": 0, "warmup": warmup,
                "readiness": {"failed": failed,
                              "failReason": "no window-ready within 30s timeout" if failed else None,
                              "l1Ms": l1, "l2Ms": l2}}

    def build_samples(self):
        samples = [self.sample(0, "pd", "warm", -1, 599.0, 601.0, warmup=True)]
        index = 1
        pd_cold = [600.0, 610.0, 590.0, 605.0]
        legacy_cold = [520.0, 530.0, 510.0, 525.0]
        pd_warm_l1 = [601.0, 609.0, 595.0, 596.0]
        pd_warm_l2 = [609.0, 617.0, 603.0, None]  # last run L2-censored (L1 reached)
        legacy_warm = [521.0, 529.0, 512.0, 524.0]
        for round_no in range(4):
            samples.append(self.sample(index, "pd", "cold", round_no, pd_cold[round_no],
                                       pd_cold[round_no] + 8))
            index += 1
            samples.append(self.sample(index, "legacy", "cold", round_no, legacy_cold[round_no],
                                       legacy_cold[round_no] + 8))
            index += 1
        for round_no in range(4):
            samples.append(self.sample(index, "pd", "warm", round_no, pd_warm_l1[round_no],
                                       pd_warm_l2[round_no]))
            index += 1
            samples.append(self.sample(index, "legacy", "warm", round_no, legacy_warm[round_no],
                                       legacy_warm[round_no] + 8))
            index += 1
        samples.append(self.sample(index, "legacy", "cold", 4, None, None, failed=True))
        return samples

    def setUp(self):
        self.samples = self.build_samples()
        self.stats = sp.assemble_stats(self.samples, bootstrap=300, seed=20261124)

    def test_warmup_is_excluded_from_every_group(self):
        for table in self.stats["byMetric"].values():
            for role, phases in table.items():
                for phase, group in phases.items():
                    self.assertNotIn(0, group["samplesRaw"])
        self.assertEqual(self.stats["byMetric"]["l1Ms"]["pd"]["cold"]["nRaw"], 4)

    def test_failed_launch_excluded_but_recorded(self):
        legacy_cold = self.stats["byMetric"]["l1Ms"]["legacy"]["cold"]
        self.assertEqual(legacy_cold["nRaw"], 4)
        self.assertEqual(legacy_cold["nSamples"], 4)
        self.assertEqual(len(self.stats["failures"]), 1)
        self.assertEqual(self.stats["failures"][0]["role"], "legacy")
        self.assertEqual(self.stats["failures"][0]["round"], 4)
        self.assertIn("timeout", self.stats["failures"][0]["reason"])

    def test_l2_censoring_is_counted_not_hidden(self):
        pd_warm = self.stats["byMetric"]["l2Ms"]["pd"]["warm"]
        self.assertEqual(pd_warm["nRaw"], 3)
        self.assertEqual(pd_warm["nMissing"], 1)
        self.assertEqual(self.stats["byMetric"]["l1Ms"]["pd"]["warm"]["nRaw"], 4)

    def test_median_values_are_exact(self):
        self.assertEqual(self.stats["byMetric"]["l1Ms"]["pd"]["cold"]["stats"]["median"], 602.5)
        self.assertEqual(self.stats["byMetric"]["l1Ms"]["legacy"]["cold"]["stats"]["median"], 522.5)

    def test_paired_per_round_differences_pd_minus_legacy(self):
        paired = self.stats["paired"]["l1Ms"]["cold"]
        self.assertTrue(paired["available"])
        self.assertEqual(paired["rounds"], [0, 1, 2, 3])
        self.assertEqual(paired["diffsPdMinusLegacy"], [80.0, 80.0, 80.0, 80.0])
        self.assertEqual(paired["median"], 80.0)
        self.assertEqual(paired["ci95Median"], [80.0, 80.0])

    def test_paired_unavailable_for_single_role_runs(self):
        samples = [self.sample(0, "pd", "cold", 0, 600.0, 610.0)]
        stats = sp.assemble_stats(samples, bootstrap=50, seed=1)
        self.assertFalse(stats["paired"]["l1Ms"]["cold"]["available"])
        self.assertIn("two roles", stats["paired"]["l1Ms"]["cold"]["reason"])

    def test_machine_readable_block_shape(self):
        expected_top = {"byMetric", "paired", "failures", "warmupFailures"}
        self.assertTrue(expected_top <= set(self.stats))
        for metric in ("l1Ms", "l2Ms"):
            self.assertIn(metric, self.stats["byMetric"])
            for role, phases in self.stats["byMetric"][metric].items():
                for phase, group in phases.items():
                    self.assertTrue({"nRaw", "nKept", "nSamples", "nMissing", "outlierRule",
                                     "excluded", "stats", "ci95Median", "samplesRaw",
                                     "samplesKept"} <= set(group))


if __name__ == "__main__":
    raise SystemExit(unittest.main())

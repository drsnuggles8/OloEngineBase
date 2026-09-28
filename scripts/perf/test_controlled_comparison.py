"""Calibration and negative controls for the run-level decision rule."""
import math
import random
import unittest

from controlled_comparison import compare_pairs, median_interval


class ControlledComparisonTest(unittest.TestCase):
    def compare(self, a, b, **kwargs):
        return compare_pairs(a, b, tolerance=0.05, alpha=0.01,
                             max_interval_width=0.15, **kwargs)

    def test_injected_slowdown_survives_common_drift_and_one_outlier(self):
        a = [10 + i for i in range(24)]
        b = [v * 1.20 for v in a]
        b[5] *= 4
        self.assertEqual(self.compare(a, b)["status"], "regression")

    def test_no_change_false_alarm_calibration(self):
        rng = random.Random(1339)
        outcomes = {"pass": 0, "regression": 0, "inconclusive": 0}
        for _ in range(1000):
            a, b = [], []
            for pair in range(24):
                drift = 10 + pair / 2
                a.append(drift * math.exp(rng.gauss(0, 0.01)))
                b.append(drift * math.exp(rng.gauss(0, 0.01)))
            outcomes[self.compare(a, b)["status"]] += 1
        self.assertEqual(outcomes["regression"], 0, outcomes)
        self.assertEqual(outcomes["pass"], 1000, outcomes)

    def test_interval_coverage_under_null_at_decision_boundary(self):
        rng = random.Random(1488)
        misses = 0
        for _ in range(1000):
            lo, hi = median_interval([rng.gauss(0, 1) for _ in range(24)], 0.01)
            misses += not lo <= 0 <= hi
        self.assertLessEqual(misses, 20)

    def test_noise_is_inconclusive(self):
        self.assertEqual(self.compare([10] * 24, [5, 20] * 12)["status"], "inconclusive")

    def test_zero_deadline_misses_are_supported(self):
        result = self.compare([0] * 24, [0.1] * 24, relative=False)
        self.assertEqual(result["status"], "regression")

    def test_invalid_and_insufficient_measurements_are_rejected(self):
        for a, b in [([], []), ([1], [1]), ([1] * 24, [float('nan')] * 24),
                     ([0] * 24, [1] * 24), ([1] * 24, [1] * 23)]:
            with self.assertRaises(ValueError):
                self.compare(a, b)


if __name__ == '__main__':
    unittest.main()

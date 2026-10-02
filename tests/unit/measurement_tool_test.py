"""验证测量口径的计算，避免 P95 或缺失样本被误标通过。"""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("measurement", Path(__file__).parents[2] / "scripts" / "measure-performance.py")
measurement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measurement)

class MeasurementTest(unittest.TestCase):
    def test_p95_thirty_is_twenty_ninth(self):
        self.assertEqual(measurement.nearest_rank(list(range(1, 31))), 29)
        self.assertEqual(measurement.nearest_rank([5, 1]), 5)
        self.assertIsNone(measurement.nearest_rank([]))

    def test_invalid_sessions_and_proxy_are_not_passes(self):
        rows = [{"cold": True, "nf01_proxy_ms": 20, "interactive_ns": "2", "outcome": 2},
                {"cold": True, "nf01_proxy_ms": 1, "outcome": 3},
                {"cold": False, "nf01_proxy_ms": 30, "interactive_ns": "3", "outcome": 8}]
        result = measurement.nf01_summary(rows)
        self.assertEqual(result["cold"]["count"], 1)
        self.assertEqual(result["warm"]["count"], 0)
        self.assertFalse(result["presentation_validated"])
        self.assertIsNone(result["nf01_pass"])

    def test_cpu_is_one_core_with_all_processes(self):
        before = {"app": [{"pid": 1, "birth": "a", "cpu_ns": "100", "error": 0},
                          {"pid": 2, "birth": "b", "cpu_ns": "200", "error": 0}]}
        after = {"app": [{"pid": 1, "birth": "a", "cpu_ns": "200000100", "error": 0},
                         {"pid": 2, "birth": "b", "cpu_ns": "300000200", "error": 0}]}
        delta, incomplete = measurement.cpu_delta(before, after, "app")
        self.assertEqual(delta, 500000000)
        self.assertEqual(delta / 1000000000 * 100, 50)
        self.assertFalse(incomplete)

    def test_copied_sessions_preserve_legacy_filter(self):
        rows = [{"cold": False, "nf01_proxy_ms": 20, "interactive_ns": "2", "outcome": outcome}
                for outcome in (1, 2, 10)]
        rows += [{"cold": False, "nf01_proxy_ms": 1, "outcome": 10},
                 {"cold": False, "nf01_proxy_ms": 1, "interactive_ns": "2", "outcome": 9}]
        result = measurement.nf01_summary(rows)
        self.assertEqual(result["warm"]["count"], 3)
        self.assertEqual(result["warm"]["p95_proxy_ms"], 20)

    def test_process_exit_marks_incomplete_accounting(self):
        before = {"app": [{"pid": 1, "birth": "a", "cpu_ns": "100", "error": 0}]}
        delta, incomplete = measurement.cpu_delta(before, {"app": []}, "app")
        self.assertTrue(incomplete)
        self.assertEqual(delta, 0)

if __name__ == "__main__":
    unittest.main()

"""验证测量口径的计算，避免 P95 或缺失样本被误标通过。"""
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

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

    def test_launchservices_uses_bundle_and_new_pid(self):
        app = Path("/tmp/构建目录/WaibuSnap.app/Contents/MacOS/WaibuSnap")
        with patch.object(measurement, "application_pids", side_effect=[set(), {123}]), \
             patch.object(measurement.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as run:
            process = measurement.start_nf01_process(app, Path("stdout.log"), Path("stderr.log"),
                                                    ["--test-mode", "--test-count", "2"])
        self.assertEqual(process.pid, 123)
        self.assertEqual(run.call_args.args[0], ["open", "-n", str(app.parents[2]),
            "--stdout", "stdout.log", "--stderr", "stderr.log", "--args",
            "--test-mode", "--test-count", "2"])
        with patch.object(measurement, "application_pids", return_value={99}), \
             patch.object(measurement.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "退出已有"):
                measurement.start_nf01_process(app, Path("stdout"), Path("stderr"), [])
            run.assert_not_called()

    def test_launchservices_handle_wait_and_signal(self):
        process = measurement.LaunchServicesProcess(123)
        with patch.object(measurement.os, "kill") as kill:
            self.assertIsNone(process.poll())
            with self.assertRaises(subprocess.TimeoutExpired):
                process.wait(timeout=0)
            process.send_signal(signal.SIGTERM)
            kill.assert_called_with(123, signal.SIGTERM)
        with patch.object(measurement.os, "kill", side_effect=ProcessLookupError):
            self.assertEqual(process.wait(timeout=0), 0)
            process.send_signal(signal.SIGTERM)
        with patch.object(measurement.os, "kill", side_effect=PermissionError):
            self.assertIsNone(process.poll())

    def test_nf01_incomplete_capture_and_timeout_include_stderr(self):
        args = SimpleNamespace(cold=1, warm=0, stable_seconds=10, interval_seconds=1)
        # 进程消失不能证明成功；缺日志、缺会话、缺终点及无效码均须失败。
        for rows in (None, [], [{"outcome": 3}], [{"outcome": 9, "interactive_ns": "2"}]):
            with self.subTest(rows=rows), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                if rows is not None:
                    (output / "cold-00.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))
                process = Mock()
                process.poll.return_value = 0
                with patch.object(measurement, "start_nf01_process", return_value=process), \
                     patch.object(measurement, "collect_stability", return_value={"stable": True}):
                    with self.assertRaisesRegex(RuntimeError, "cold-00-stderr.log"):
                        measurement.measure_nf01(args, Path("app"), Path("probe"), output)
        with tempfile.TemporaryDirectory() as directory:
            process = Mock()
            process.poll.return_value = 0
            process.wait.side_effect = subprocess.TimeoutExpired("WaibuSnap", 51)
            with patch.object(measurement, "start_nf01_process", return_value=process), \
                 patch.object(measurement, "collect_stability", return_value={"stable": True}):
                with self.assertRaisesRegex(RuntimeError, "cold-00-stderr.log"):
                    measurement.measure_nf01(args, Path("app"), Path("probe"), Path(directory))

    def test_launchservices_pid_startup_timeout_and_ambiguity(self):
        app = Path("/tmp/WaibuSnap.app/Contents/MacOS/WaibuSnap")
        with patch.object(measurement, "application_pids", return_value=set()), \
             patch.object(measurement.subprocess, "run", return_value=SimpleNamespace(returncode=0)), \
             patch.object(measurement.time, "monotonic", side_effect=[0, 11]):
            with self.assertRaisesRegex(RuntimeError, "10 秒"):
                measurement.start_nf01_process(app, Path("stdout"), Path("stderr"), [])
        with patch.object(measurement, "application_pids", side_effect=[set(), {123, 456}]), \
             patch.object(measurement.subprocess, "run", return_value=SimpleNamespace(returncode=0)):
            with self.assertRaisesRegex(RuntimeError, "多个"):
                measurement.start_nf01_process(app, Path("stdout"), Path("stderr"), [])

if __name__ == "__main__":
    unittest.main()

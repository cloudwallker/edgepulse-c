"""Black-box contract checks for the actual C simulator executable."""
import ctypes
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

if os.name == "nt":
    ctypes.windll.kernel32.SetErrorMode(0x0002)

BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path("build/edgepulse").resolve()
sys.argv = sys.argv[:1]


class SimulatorCLI(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([str(BINARY), *args], text=True, capture_output=True, timeout=30)

    def summary(self, *args):
        result = self.run_cli("--demo", "--json", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertTrue(result.stdout.lstrip().startswith("{"), "JSON summary is missing")
        data = json.loads(result.stdout)
        self.assertIs(data["accounting_ok"], True)
        self.assertEqual(data["sampled"], data["sent"] + data["dropped"] + data["reset_dropped"] + data["pending"])
        self.assertEqual(data["attempts"], data["sampled"] + data["sensor_errors"])
        self.assertEqual(data["sent"], data["wire_received"])
        self.assertEqual(data["wire_rejected"], 0)
        self.assertLessEqual(data["queue_peak"], 16)
        self.assertEqual(data["commands_applied"], 1)
        self.assertEqual(data["duplicate_commands"], 1)
        self.assertEqual(data["store_writes"], 1)
        return data

    def test_default_fault_campaign_has_real_effects(self):
        data = self.summary()
        self.assertEqual(data["seed"], 20261023)
        self.assertEqual(data["ticks"], 12000)
        self.assertGreater(data["sent"], 100)
        self.assertGreater(data["dropped"], 0)
        self.assertGreater(data["reset_dropped"], 0)
        self.assertGreater(data["sensor_errors"], 0)
        self.assertEqual(data["resets"], 1)
        self.assertEqual(data["pending"], 0)
        self.assertEqual(data["period_ms"], 10)
        self.assertEqual(data["threshold_mc"], 30000)
        self.assertIs(data["persistent"], False)

    def test_seed_reproduces_byte_stream(self):
        first = self.summary("--seed", "7")
        self.assertEqual(first, self.summary("--seed", "7"))
        self.assertNotEqual(first["telemetry_checksum"], self.summary("--seed", "8")["telemetry_checksum"])

    def test_numeric_boundaries(self):
        self.assertEqual(self.summary("--ticks", "10", "--seed", "4294967295")["ticks"], 10)
        self.assertEqual(self.summary("--ticks", "1000000", "--seed", "1")["ticks"], 1000000)

    def test_invalid_values_fail(self):
        bad = [
            ("--seed", "0"), ("--seed", "-1"), ("--seed", "+1"),
            ("--seed", "4294967296"), ("--seed", "1junk"),
            ("--seed", " 1"), ("--seed", ""), ("--seed",),
            ("--ticks", "9"), ("--ticks", "1000001"), ("--ticks", "-5"),
            ("--ticks", "18446744073709551616"), ("--ticks", "1.5"),
            ("--ticks",), ("--unknown",), ("bare",),
        ]
        for args in bad:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)

    def test_help_and_readable_demo(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--seed", result.stdout)
        result = self.run_cli("--demo", "--ticks", "100")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("EdgePulse C", result.stdout)
        self.assertIn('"accounting_ok":true', result.stdout)

    def config_result(self, *args):
        result = self.run_cli("config", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        data = json.loads(result.stdout)
        self.assertIs(data["persistent"], True)
        self.assertGreaterEqual(data["generation"], 1)
        return data

    def test_config_survives_independent_processes_and_demo(self):
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "device.bin"
            saved = self.config_result("set", str(path), "25", "-12500")
            before = path.read_bytes()
            shown = self.config_result("show", str(path))
            self.assertEqual(shown, saved)
            self.assertEqual(path.read_bytes(), before, "show must not write the file")
            demo = self.summary("--store", str(path))
            self.assertTrue(demo["persistent"])
            self.assertEqual((demo["period_ms"], demo["threshold_mc"]), (25, -12500))
            after = self.config_result("show", str(path))
            self.assertEqual((after["period_ms"], after["threshold_mc"]), (25, -12500))
            self.assertGreater(after["generation"], shown["generation"])

    def test_config_accepts_unicode_space_paths_and_numeric_limits(self):
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "设备 配置.bin"
            first = self.config_result("set", str(path), "10", "-40000")
            self.assertEqual((first["period_ms"], first["threshold_mc"]), (10, -40000))
            second = self.config_result("set", str(path), "60000", "125000")
            self.assertEqual((second["period_ms"], second["threshold_mc"]), (60000, 125000))
            self.assertGreater(second["generation"], first["generation"])
            shown = self.config_result("show", str(path))
            self.assertEqual(shown, second)
            demo = self.summary("--store", str(path))
            self.assertEqual((demo["period_ms"], demo["threshold_mc"]), (60000, 125000))

    def test_config_invalid_arguments_do_not_touch_files(self):
        bad_values = [
            ("9", "0"), ("60001", "0"), ("-1", "0"), ("+10", "0"),
            ("10junk", "0"), ("", "0"), ("10", "-40001"), ("10", "125001"),
            ("10", "+1"), ("10", "-"), ("10", ""), ("10", " 1"),
            ("10", "1.5"), ("10", "4294967296"), ("10", "-4294967296"),
        ]
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "must-not-create.bin"
            for values in bad_values:
                with self.subTest(values=values):
                    result = self.run_cli("config", "set", str(path), *values)
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, "")
                    self.assertFalse(path.exists())
            path.write_bytes(b"keep unrelated data")
            cases = [
                ("config",), ("config", "set", str(path), "25"),
                ("config", "set", str(path), "25", "0", "extra"),
                ("config", "show", str(path), "extra"),
                ("config", "unknown", str(path)),
                ("config", "show", ""),
                ("config", "set", "", "25", "0"),
                ("--store",), ("--store", ""), ("--store", "--json"),
                ("--store", str(path), "--store", str(path)),
                ("--store", str(path), "config", "show", str(path)),
            ]
            for args in cases:
                with self.subTest(args=args):
                    result = self.run_cli(*args)
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual(path.read_bytes(), b"keep unrelated data")

    def test_missing_and_unknown_config_files_fail_without_success_output(self):
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "private-name.bin"
            for args in [("config", "show", str(path)), ("--store", str(path), "--json")]:
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertNotIn(str(path), result.stderr)
                self.assertFalse(path.exists())
            for content in [b"", b"x" * 47, b"x" * 48, b"x" * 49]:
                path.write_bytes(content)
                for args in [("config", "set", str(path), "25", "0"),
                             ("config", "show", str(path)), ("--store", str(path), "--json")]:
                    with self.subTest(size=len(content), args=args):
                        result = self.run_cli(*args)
                        self.assertEqual(result.returncode, 1)
                        self.assertEqual(result.stdout, "")
                        self.assertNotIn(str(path), result.stderr)
                        self.assertEqual(path.read_bytes(), content)


if __name__ == "__main__":
    unittest.main(verbosity=2)

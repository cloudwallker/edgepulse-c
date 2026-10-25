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
        self.assertEqual(data["sampled"], data["sent"] + data["dropped"] + data["reset_dropped"] + data["pending"] + data.get("delivery_timeouts", 0))
        self.assertEqual(data["attempts"], data["sampled"] + data["sensor_errors"])
        if data.get("reliable", False):
            self.assertEqual(data["sent"], data["acknowledged"])
            self.assertLessEqual(data["acknowledged"], data["receiver_unique"])
            self.assertEqual(data["wire_received"], data["receiver_unique"] + data["receiver_duplicates"])
            self.assertEqual(data["ack_received"], data["acknowledged"] + data["invalid_acks"])
            self.assertGreaterEqual(data["tx_attempts"], data["wire_transmitted"])
            self.assertLessEqual(data["retransmissions"], data["tx_attempts"])
            self.assertGreaterEqual(data["wire_rejected"], data["data_corrupted"])
            self.assertGreaterEqual(data["ack_rejected"], data["ack_corrupted_delivered"])
            self.assertEqual(data["receiver_rejected"], 0)
        else:
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
        self.assertEqual((data["sampled"], data["sent"], data["dropped"], data["telemetry_checksum"]),
                         (1159, 593, 550, 3411655967))

    def test_reliable_fault_campaign_retries_without_duplicate_delivery(self):
        data = self.summary("--reliable")
        self.assertIs(data["reliable"], True)
        for metric in ("acknowledged", "retransmissions", "delivery_timeouts", "receiver_duplicates",
                       "invalid_acks", "data_lost", "data_corrupted", "ack_lost", "ack_corrupted_delivered"):
            self.assertGreater(data[metric], 0, metric)
        self.assertGreater(data["wire_received"], data["receiver_unique"])

    def test_reliable_seed_reproduces_faults_and_wire_bytes(self):
        first = self.summary("--reliable", "--seed", "7")
        self.assertEqual(first, self.summary("--reliable", "--seed", "7"))
        self.assertNotEqual(first["telemetry_checksum"],
                            self.summary("--reliable", "--seed", "8")["telemetry_checksum"])

    def test_reliable_short_and_long_campaigns_reconcile(self):
        for ticks, seed in [("10", "4294967295"), ("1000000", "1")]:
            data = self.summary("--reliable", "--ticks", ticks, "--seed", seed)
            self.assertTrue(data["reliable"])
            self.assertEqual(data["ticks"], int(ticks))

    def test_reliable_mode_uses_persistent_configuration(self):
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "可靠 配置.epcfg"
            for period in ("25", "60000"):
                saved = self.config_result("set", str(path), period, "-12500")
                data = self.summary("--reliable", "--store", str(path))
                self.assertTrue(data["reliable"] and data["persistent"])
                self.assertEqual((data["period_ms"], data["threshold_mc"]), (int(period), -12500))
                shown = self.config_result("show", str(path))
                self.assertEqual(shown["generation"], saved["generation"] + 1)

    def test_reliable_option_errors_do_not_touch_storage(self):
        with tempfile.TemporaryDirectory(prefix="ep-cli-", dir=BINARY.parent) as folder:
            path = Path(folder) / "keep.epcfg"
            path.write_bytes(b"keep existing bytes")
            result = self.run_cli("--reliable", "--reliable", "--store", str(path))
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
            self.assertEqual(path.read_bytes(), b"keep existing bytes")
            result = self.run_cli("--reliable", "--store", str(path), "--json")
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, "")
            self.assertNotIn(str(path), result.stderr)
            self.assertEqual(path.read_bytes(), b"keep existing bytes")

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

"""Black-box contract checks for the actual C simulator executable."""
import ctypes
import json
import os
import subprocess
import sys
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


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Real child-process termination and file recovery contract tests."""
import ctypes
import binascii
import json
import os
from pathlib import Path
import queue
import subprocess
import struct
import sys
import tempfile
import threading
import unittest

if os.name == "nt":
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
WORKER = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0], *sys.argv[2:]]


class FileStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="edgepulse-file-", dir=WORKER.parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "配置 with space.bin"

    def run_worker(self, *args):
        return subprocess.run([str(WORKER), *map(str, args)], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=10)

    def successful(self, *args):
        result = self.run_worker(*args)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result

    def read_config(self, path=None):
        return json.loads(self.successful("show", path or self.path).stdout)

    def establish_old(self):
        self.successful("set", self.path, 100, 30000)
        self.successful("set", self.path, 250, 30000)
        return self.path.read_bytes()

    def blocked_child(self, *args):
        process = subprocess.Popen([str(WORKER), *map(str, args)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", errors="replace")
        messages = queue.Queue()
        threading.Thread(target=lambda: messages.put(process.stdout.readline()), daemon=True).start()
        try:
            self.assertEqual(messages.get(timeout=10), "READY\n")
        except BaseException:
            process.kill()
            output, error = process.communicate(timeout=10)
            self.fail("Worker did not reach the requested boundary: " + output + error)
        return process

    def kill_child(self, process):
        process.kill()
        output, error = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0)
        self.assertNotIn("ERROR: AddressSanitizer", error)
        self.assertNotIn("runtime error:", error)
        return output

    def test_roundtrip_unicode_and_read_has_no_writes(self):
        self.successful("set", self.path, 25, -40000)
        before = self.path.read_bytes()
        self.assertEqual(len(before), 48)
        value = self.read_config()
        self.assertEqual((value["period_ms"], value["threshold_mc"]), (25, -40000))
        self.assertEqual(before, self.path.read_bytes())

    def test_unrecognized_files_are_preserved(self):
        for data in (b"", b"x" * 47, b"x" * 49, b"x" * 48, b"\xff" * 48):
            with self.subTest(length=len(data), prefix=data[:1]):
                self.path.write_bytes(data)
                self.assertNotEqual(self.run_worker("set", self.path, 25, 30000).returncode, 0)
                self.assertEqual(self.path.read_bytes(), data)

    def test_corrupt_slot_falls_back_and_two_bad_slots_are_preserved(self):
        self.establish_old()
        data = bytearray(self.path.read_bytes())
        # Three commits: initial defaults slot0, first set slot1, second set slot0.
        data[8] ^= 0x40
        self.path.write_bytes(data)
        self.assertEqual(self.read_config()["period_ms"], 100)
        data[24 + 8] ^= 0x40
        self.path.write_bytes(data)
        self.assertNotEqual(self.run_worker("set", self.path, 25, 30000).returncode, 0)
        self.assertEqual(self.path.read_bytes(), data)

    def test_missing_path_and_directory(self):
        self.assertNotEqual(self.run_worker("show", self.path).returncode, 0)
        self.assertFalse(self.path.exists())
        self.assertNotEqual(self.run_worker("set", self.root / "missing" / "state", 25, 30000).returncode, 0)
        self.assertNotEqual(self.run_worker("set", self.root, 25, 30000).returncode, 0)

    def test_final_component_symlink_is_rejected(self):
        self.establish_old()
        link = self.root / "link.bin"
        try:
            link.symlink_to(self.path)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account has no symlink creation privilege")
            raise
        before = self.path.read_bytes()
        self.assertNotEqual(self.run_worker("set", link, 25, 30000).returncode, 0)
        self.assertEqual(before, self.path.read_bytes())

    def test_readonly_file_is_not_modified(self):
        before = self.establish_old()
        self.path.chmod(0o444)
        try:
            if os.name != "nt" and hasattr(os, "geteuid") and os.geteuid() == 0:
                self.skipTest("Root bypasses ordinary file write permissions")
            self.assertNotEqual(self.run_worker("set", self.path, 25, 30000).returncode, 0)
            self.assertEqual(before, self.path.read_bytes())
        finally:
            self.path.chmod(0o600)

    @staticmethod
    def record(period, threshold, generation, version=1):
        prefix = struct.pack("<2sBBIIi5x", b"EP", version, 0, generation, period, threshold)
        return prefix + struct.pack("<H", binascii.crc_hqx(prefix, 0xFFFF)) + b"\xa5"

    def test_unknown_version_with_valid_crc_is_preserved(self):
        data = self.record(100, 30000, 1, version=9) * 2
        self.path.write_bytes(data)
        self.assertNotEqual(self.run_worker("set", self.path, 25, 30000).returncode, 0)
        self.assertEqual(self.path.read_bytes(), data)

    def test_generation_rollover_is_persisted(self):
        self.path.write_bytes(self.record(100, 30000, 0xFFFFFFFF) + b"\xff" * 24)
        self.successful("set", self.path, 250, -5000)
        data = self.read_config()
        self.assertEqual(data, {"period_ms": 250, "threshold_mc": -5000, "generation": 0})

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX FIFO only")
    def test_fifo_is_rejected_without_blocking(self):
        os.mkfifo(self.path)
        self.assertNotEqual(self.run_worker("show", self.path).returncode, 0)

    def test_lock_contention_and_termination_releases_lock(self):
        before = self.establish_old()
        process = self.blocked_child("hold", self.path)
        try:
            competing = self.run_worker("show", self.path)
            self.assertNotEqual(competing.returncode, 0)
            self.assertIn("busy", competing.stderr)
            competing = self.run_worker("set", self.path, 25, -5000)
            self.assertNotEqual(competing.returncode, 0)
            self.assertIn("busy", competing.stderr)
        finally:
            self.kill_child(process)
        self.assertEqual(self.read_config()["period_ms"], 250)
        self.assertEqual(before, self.path.read_bytes())

    def test_27_real_process_kills_recover_complete_configuration(self):
        before = self.establish_old()
        boundaries = [("clear", 0), *[("payload", i) for i in range(1, 24)],
                      ("payload-sync", 0), ("commit", 0), ("committed", 0)]
        for stage, step in boundaries:
            with self.subTest(stage=stage, step=step):
                self.path.write_bytes(before)
                process = self.blocked_child("probe", self.path, stage, step, "kill")
                self.kill_child(process)
                data = self.read_config()
                actual = (data["period_ms"], data["threshold_mc"])
                if stage in ("commit", "committed"):
                    self.assertEqual(actual, (750, -5000))
                else:
                    self.assertEqual(actual, (250, 30000))
                self.assertEqual(len(self.path.read_bytes()), 48)

    def test_28_write_and_sync_failures_do_not_commit_application_state(self):
        before = self.establish_old()
        failures = [("write", i) for i in range(25)] + [("sync", i) for i in range(3)]
        for stage, step in failures:
            with self.subTest(stage=stage, step=step):
                self.path.write_bytes(before)
                result = self.successful("probe", self.path, stage, step, "fail")
                self.assertIn("failure_verified", result.stdout)
                data = self.read_config()
                actual = (data["period_ms"], data["threshold_mc"])
                self.assertIn(actual, [(250, 30000), (750, -5000)])
                if stage != "sync" or step != 2:
                    self.assertEqual(actual, (250, 30000))

    def test_short_reads_and_writes_are_completed(self):
        self.successful("short", self.path, 25, 125000)
        self.assertEqual(self.read_config()["period_ms"], 25)
        self.successful("short", self.path, 45, -1000)
        self.assertEqual(self.read_config()["threshold_mc"], -1000)

    def test_read_error_and_closed_store_refuse_saves(self):
        before = self.establish_old()
        self.assertNotEqual(self.run_worker("openfail", self.path).returncode, 0)
        self.assertEqual(before, self.path.read_bytes())
        self.assertIn("closed_save_rejected", self.successful("closed", self.path).stdout)
        self.assertEqual(before, self.path.read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)

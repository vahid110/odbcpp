"""Offline scheduling checks; these make no product qualification claim."""

import pathlib
import os
import subprocess
import sys
import tempfile
import time
import unittest


WRAPPER = pathlib.Path(__file__).resolve().parents[2] / "tools/development/with_build_slot.py"


class BuildSlotTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.lock = self.root / "build.lock"

    def argv(self, code, wait=0):
        return [sys.executable, str(WRAPPER), "--lock-file", str(self.lock),
                "--wait-seconds", str(wait), "--", sys.executable, "-c", code]

    def hold(self):
        ready = self.root / "ready"
        code = f"from pathlib import Path; import sys; Path({str(ready)!r}).touch(); sys.stdin.buffer.read(1)"
        proc = subprocess.Popen(self.argv(code), stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def cleanup():
            if proc.poll() is None:
                proc.communicate(b"x", timeout=5)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 5
        while not ready.exists():
            if proc.poll() is not None or time.monotonic() >= deadline:
                self.fail("holding command did not start")
            time.sleep(0.01)
        return proc

    def test_contender_cannot_run_until_release(self):
        owner = self.hold()
        marker = self.root / "contender"
        code = f"from pathlib import Path; Path({str(marker)!r}).touch()"
        blocked = subprocess.run(self.argv(code), capture_output=True, timeout=5)
        self.assertEqual(blocked.returncode, 75)
        self.assertFalse(marker.exists())
        owner.communicate(b"x", timeout=5)
        self.assertEqual(owner.returncode, 0)
        admitted = subprocess.run(self.argv(code), capture_output=True, timeout=5)
        self.assertEqual(admitted.returncode, 0)
        self.assertTrue(marker.exists())

    def test_waiting_command_runs_after_owner(self):
        owner = self.hold()
        marker = self.root / "waiter"
        waiter = subprocess.Popen(self.argv(
            f"from pathlib import Path; Path({str(marker)!r}).touch()", wait=5),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: waiter.communicate(timeout=6))
        self.assertFalse(marker.exists())
        owner.communicate(b"x", timeout=5)
        waiter.communicate(timeout=6)
        self.assertEqual(waiter.returncode, 0)
        self.assertTrue(marker.exists())

    def test_failed_command_releases_slot_and_preserves_exit(self):
        failed = subprocess.run(self.argv("import sys; sys.exit(17)"),
                                capture_output=True, timeout=5)
        self.assertEqual(failed.returncode, 17)
        successor = subprocess.run(self.argv("pass"), capture_output=True, timeout=5)
        self.assertEqual(successor.returncode, 0)

    def test_invalid_wait_never_starts_command(self):
        marker = self.root / "invalid"
        for wait in (-1, "nan", "inf"):
            with self.subTest(wait=wait):
                result = subprocess.run(self.argv(
                    f"from pathlib import Path; Path({str(marker)!r}).touch()", wait=wait),
                    capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(marker.exists())

    def test_symlink_lock_refused(self):
        target = self.root / "target"
        target.write_text("retained")
        self.lock.symlink_to(target)
        result = subprocess.run(self.argv("pass"), capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "retained")

    def test_nonregular_lock_refused_without_blocking(self):
        os.mkfifo(self.lock)
        result = subprocess.run(self.argv("pass"), capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()

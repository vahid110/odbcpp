"""A canary fingerprint must cover every trusted fallback policy input."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class FingerprintTests(unittest.TestCase):
    def test_changing_either_trusted_policy_stales_the_canary_fingerprint(self):
        repository = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory)
            script = Path(".github/review/fingerprint.sh")
            policies = ["AGENTS.md", "docs/adr/0001-development-process.md"]
            workflows = [path.relative_to(repository)
                         for path in repository.glob(".github/workflows/agent-review*.yml")]
            for relative in [script, *policies, *workflows]:
                destination = checkout / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(repository / relative, destination)

            def fingerprint():
                return subprocess.check_output([str(checkout / script)], text=True).strip()

            baseline = fingerprint()
            for name in policies:
                with self.subTest(name=name):
                    policy = checkout / name
                    original = policy.read_bytes()
                    policy.write_bytes(original + b"\nChanged for this test.\n")
                    self.assertNotEqual(baseline, fingerprint())
                    policy.write_bytes(original)
                    self.assertEqual(baseline, fingerprint())

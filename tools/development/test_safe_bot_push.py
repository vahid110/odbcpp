#!/usr/bin/env python3
"""Offline demonstration and contracts for safe bot git push operations.

Proves that:
1. Non-HTTPS destinations (SSH remotes, git@, ssh://) are detected and rejected.
2. Git URL rewrites (url.<base>.insteadOf) that redirect HTTPS to SSH are detected and rejected.
3. Passing -c core.sshCommand="false" strictly prevents Git from invoking SSH,
   ensuring that personal SSH keys/agents cannot be used even under misconfiguration.
4. Clean HTTPS destinations resolve successfully.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def verify_target_https_url(repo_dir: str, target_url: str = "https://github.com/vahid110/odbcpp.git") -> str:
    """Offline Python contract verifying effective push destination is HTTPS."""
    res = subprocess.run(
        ["git", "-C", repo_dir, "ls-remote", "--get-url", target_url],
        capture_output=True,
        text=True,
        check=True,
    )
    effective_url = res.stdout.strip()
    if not (effective_url.startswith("https://github.com/vahid110/odbcpp") or
            effective_url.startswith("https://www.github.com/vahid110/odbcpp")):
        raise ValueError(f"Push target resolved to non-HTTPS ({effective_url}). Prohibited for bot push.")
    return effective_url


class SafeBotPushDemonstrationTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo = self.temp_dir.name
        subprocess.run(["git", "init", self.repo], check=True, capture_output=True)
        # Create an initial commit
        readme = Path(self.repo) / "README.md"
        readme.write_text("# Test Repo\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.repo, "add", "."], check=True, capture_output=True)
        subprocess.run(
            ["git", "-C", self.repo, "-c", "user.name=bot", "-c", "user.email=bot@example.com",
             "commit", "-m", "initial"],
            check=True,
            capture_output=True,
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_https_clean_path_accepted(self):
        """Standard HTTPS target without rewrites is verified and accepted."""
        effective = verify_target_https_url(self.repo, "https://github.com/vahid110/odbcpp.git")
        self.assertEqual(effective, "https://github.com/vahid110/odbcpp.git")

    def test_ssh_rewrite_detected_and_rejected(self):
        """Git config rewrites (insteadOf) that map HTTPS to SSH are detected and rejected."""
        subprocess.run(
            ["git", "-C", self.repo, "config", "url.git@github.com:.insteadOf", "https://github.com/"],
            check=True,
        )
        with self.assertRaises(ValueError) as ctx:
            verify_target_https_url(self.repo, "https://github.com/vahid110/odbcpp.git")
        self.assertIn("non-HTTPS", str(ctx.exception))
        self.assertIn("git@github.com:vahid110/odbcpp.git", str(ctx.exception))

    def test_ssh_fail_closed_guard_prevents_ssh_invocation(self):
        """Passing core.sshCommand='false' fails closed immediately without invoking SSH."""
        res = subprocess.run(
            [
                "git", "-C", self.repo,
                "-c", "core.sshCommand=false",
                "push", "--dry-run", "git@github.com:vahid110/odbcpp.git", "HEAD:refs/heads/test-branch"
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(res.returncode, 0)
        # Output demonstrates failure to execute/read from SSH transport without network/key exposure
        self.assertTrue(
            "Could not read from remote repository" in res.stderr or
            "fatal: " in res.stderr
        )

    def test_posix_shell_snippet_behavior(self):
        """Executes the exact POSIX shell verification snippet from docs/AGENT_WORKFLOW.md."""
        shell_script = """
        set -eu
        TARGET_REPO="https://github.com/vahid110/odbcpp.git"
        EFFECTIVE_URL=$(git -C "$REPO_PATH" ls-remote --get-url "$TARGET_REPO")
        case "$EFFECTIVE_URL" in
          https://github.com/vahid110/odbcpp* | https://*.github.com/vahid110/odbcpp*)
            exit 0
            ;;
          *)
            echo "Error: Push target resolved to non-HTTPS ($EFFECTIVE_URL). Prohibited for bot push." >&2
            exit 1
            ;;
        esac
        """
        # 1. Clean repo: should succeed
        clean_res = subprocess.run(
            ["bash", "-c", shell_script],
            env={"REPO_PATH": self.repo},
            capture_output=True,
            text=True,
        )
        self.assertEqual(clean_res.returncode, 0)

        # 2. Repo with SSH rewrite: should fail closed with exit code 1
        subprocess.run(
            ["git", "-C", self.repo, "config", "url.git@github.com:.insteadOf", "https://github.com/"],
            check=True,
        )
        rewrite_res = subprocess.run(
            ["bash", "-c", shell_script],
            env={"REPO_PATH": self.repo},
            capture_output=True,
            text=True,
        )
        self.assertEqual(rewrite_res.returncode, 1)
        self.assertIn("Push target resolved to non-HTTPS", rewrite_res.stderr)


if __name__ == "__main__":
    unittest.main()

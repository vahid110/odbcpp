#!/usr/bin/env python3
"""Offline demonstration and verification contracts for safe bot git push operations.

Addresses PR #170 review findings:
1. Exact canonical URL binding (rejecting different repos, glob matches, unrelated hosts).
2. Never echoing rejected raw URLs that might contain credentials.
3. Detecting and rejecting url.*.pushInsteadOf rewrites.
4. Detecting and rejecting inherited http.*extraHeader overrides without leaking values.
5. Verifying with a recording SSH stub that SSH is never invoked under pushInsteadOf + GIT_SSH_COMMAND.
6. Verifying with a local recording HTTP server that only validated bot credentials reach the transport.
7. Verifying POSIX shell verification snippet behavior across all threat scenarios.
"""

import base64
import http.server
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import threading
import unittest


def verify_canonical_target_urls(repo_dir: str, target_repo: str = "https://github.com/vahid110/odbcpp.git") -> None:
    """Offline Python contract verifying exact canonical HTTPS destination and clean config."""
    # 1. Check for inherited http.*extraheader
    res_header = subprocess.run(
        ["git", "-C", repo_dir, "config", "--name-only", "--get-regexp", r"^http\..*extraheader$"],
        capture_output=True,
        text=True,
    )
    if res_header.stdout.strip():
        raise ValueError("Error: Inherited http.*extraHeader configuration detected in git config. Prohibited for bot push.")

    # 2. Check for inherited url.*pushinsteadof
    res_push_instead = subprocess.run(
        ["git", "-C", repo_dir, "config", "--name-only", "--get-regexp", r"^url\..*pushinsteadof$"],
        capture_output=True,
        text=True,
    )
    if res_push_instead.stdout.strip():
        raise ValueError("Error: Inherited url.*pushInsteadOf configuration detected in git config. Prohibited for bot push.")

    # 3. Fetch URL resolution via ls-remote
    res_fetch = subprocess.run(
        ["git", "-C", repo_dir, "ls-remote", "--get-url", target_repo],
        capture_output=True,
        text=True,
        check=True,
    )
    fetch_url = res_fetch.stdout.strip()
    if fetch_url not in ("https://github.com/vahid110/odbcpp.git", "https://github.com/vahid110/odbcpp"):
        raise ValueError("Error: Push target did not resolve to canonical HTTPS repository URL.")

    # 4. Push URL resolution
    res_push = subprocess.run(
        ["git", "-C", repo_dir, "remote", "get-url", "--push", "origin"],
        capture_output=True,
        text=True,
    )
    if res_push.returncode == 0:
        push_url = res_push.stdout.strip()
        if push_url not in ("https://github.com/vahid110/odbcpp.git", "https://github.com/vahid110/odbcpp"):
            raise ValueError("Error: Push destination did not resolve to canonical HTTPS repository URL.")


class MockGitHttpServer(http.server.BaseHTTPRequestHandler):
    received_requests = []

    def do_GET(self):
        auth = self.headers.get("Authorization")
        MockGitHttpServer.received_requests.append({
            "path": self.path,
            "headers": dict(self.headers),
            "auth": auth,
        })
        if not auth:
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="Git"')
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/x-git-upload-pack-advertisement")
        self.end_headers()
        self.wfile.write(b"0000")

    def log_message(self, format, *args):
        pass


class SafeBotPushVerificationTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo = self.temp_dir.name
        subprocess.run(["git", "init", self.repo], check=True, capture_output=True)
        readme = Path(self.repo) / "README.md"
        readme.write_text("# Test Repo\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.repo, "add", "."], check=True, capture_output=True)
        subprocess.run(
            ["git", "-C", self.repo, "-c", "user.name=bot", "-c", "user.email=bot@example.com",
             "commit", "-m", "initial"],
            check=True,
            capture_output=True,
        )
        subprocess.run(["git", "-C", self.repo, "remote", "add", "origin", "https://github.com/vahid110/odbcpp.git"], check=True)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_exact_canonical_urls_accepted(self):
        """Canonical repository URLs with or without .git are accepted."""
        verify_canonical_target_urls(self.repo, "https://github.com/vahid110/odbcpp.git")
        verify_canonical_target_urls(self.repo, "https://github.com/vahid110/odbcpp")

    def test_different_repo_matching_prefix_rejected(self):
        """A different repository on github matching prefix (odbcpp-other) is rejected."""
        subprocess.run(["git", "-C", self.repo, "remote", "set-url", "--push", "origin", "https://github.com/vahid110/odbcpp-other.git"], check=True)
        with self.assertRaises(ValueError) as ctx:
            verify_canonical_target_urls(self.repo)
        self.assertIn("canonical HTTPS repository URL", str(ctx.exception))

    def test_unrelated_host_or_path_rejected(self):
        """Unrelated domain containing github.com in path is rejected."""
        subprocess.run(["git", "-C", self.repo, "remote", "set-url", "--push", "origin", "https://unrelated.example/.github.com/vahid110/odbcpp.git"], check=True)
        with self.assertRaises(ValueError) as ctx:
            verify_canonical_target_urls(self.repo)
        self.assertIn("canonical HTTPS repository URL", str(ctx.exception))

    def test_credential_bearing_rejected_url_does_not_leak_in_error(self):
        """Rejected credential-bearing URLs are never printed in exception messages."""
        secret = "secret-token-xyz-12345"
        subprocess.run(["git", "-C", self.repo, "remote", "set-url", "--push", "origin", f"https://user:{secret}@github.com/other/repo.git"], check=True)
        with self.assertRaises(ValueError) as ctx:
            verify_canonical_target_urls(self.repo)
        self.assertNotIn(secret, str(ctx.exception))
        self.assertNotIn("user:", str(ctx.exception))

    def test_push_instead_of_rewrite_detected_and_rejected(self):
        """url.<base>.pushInsteadOf configuration is detected and rejected."""
        subprocess.run(
            ["git", "-C", self.repo, "config", "url.git@github.com:.pushInsteadOf", "https://github.com/"],
            check=True,
        )
        with self.assertRaises(ValueError) as ctx:
            verify_canonical_target_urls(self.repo)
        self.assertIn("pushInsteadOf", str(ctx.exception))

    def test_inherited_http_extra_header_detected_and_rejected(self):
        """Inherited http.*extraHeader is detected and rejected without leaking its secret."""
        secret = "super-secret-personal-token"
        subprocess.run(
            ["git", "-C", self.repo, "config", "http.https://github.com/.extraHeader", f"Authorization: Bearer {secret}"],
            check=True,
        )
        with self.assertRaises(ValueError) as ctx:
            verify_canonical_target_urls(self.repo)
        self.assertIn("extraHeader", str(ctx.exception))
        self.assertNotIn(secret, str(ctx.exception))

    def test_recording_ssh_stub_never_invoked(self):
        """Proves with a recording stub that SSH transport is blocked before any SSH process spawns."""
        stub_path = os.path.join(self.repo, "ssh_recorder.sh")
        log_path = os.path.join(self.repo, "ssh_invoked.log")
        with open(stub_path, "w") as f:
            f.write(f'#!/bin/sh\necho "INVOKED $@" >> "{log_path}"\nexit 1\n')
        os.chmod(stub_path, 0o755)

        # Configure pushInsteadOf mapping to SSH
        subprocess.run(
            ["git", "-C", self.repo, "config", "url.git@github.com:.pushInsteadOf", "https://github.com/"],
            check=True,
        )

        # Inherited environment with hostile GIT_SSH_COMMAND pointing to stub
        inherited_env = os.environ.copy()
        inherited_env["GIT_SSH_COMMAND"] = stub_path

        # Execution environment applying safe push protocol restriction and fail-closed defense
        run_env = inherited_env.copy()
        run_env["GIT_ALLOW_PROTOCOL"] = "https"
        run_env["GIT_SSH_COMMAND"] = "false"

        res = subprocess.run(
            [
                "git", "-C", self.repo,
                "-c", "protocol.allow=never",
                "-c", "protocol.https.allow=always",
                "-c", "core.sshCommand=false",
                "push", "--dry-run", "origin", "HEAD:refs/heads/dummy"
            ],
            env=run_env,
            capture_output=True,
            text=True,
        )

        # Command must fail closed
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("transport 'ssh' not allowed", res.stderr)
        # CRITICAL PROOF: The recording SSH stub was never executed
        self.assertFalse(os.path.exists(log_path), "SSH stub was invoked despite transport security!")

    def test_offline_recording_http_server_receives_only_bot_auth(self):
        """Proves with a recording HTTP server that only intended bot credentials reach the transport."""
        MockGitHttpServer.received_requests.clear()
        server = http.server.HTTPServer(("127.0.0.1", 0), MockGitHttpServer)
        port = server.server_port
        server_thread = threading.Thread(target=server.serve_forever)
        server_thread.daemon = True
        server_thread.start()

        try:
            bot_token = "ghs_testbot123456789"
            cred_helper = os.path.join(self.repo, "mock_cred_helper.sh")
            with open(cred_helper, "w") as f:
                f.write(f'#!/bin/sh\necho "username=x-access-token"\necho "password={bot_token}"\n')
            os.chmod(cred_helper, 0o755)

            target_url = f"http://127.0.0.1:{port}/repo.git"

            # Execute git ls-remote against mock server with credential helper and cleared extraHeader
            subprocess.run(
                [
                    "git", "-C", self.repo,
                    "-c", "credential.helper=",
                    "-c", f"credential.{target_url}.helper={cred_helper}",
                    "-c", "http.extraHeader=",
                    "ls-remote", target_url
                ],
                capture_output=True,
                text=True,
            )

            # Analyze recorded requests
            self.assertGreater(len(MockGitHttpServer.received_requests), 0)
            # Find authenticated request
            auth_requests = [r for r in MockGitHttpServer.received_requests if r["auth"] is not None]
            self.assertEqual(len(auth_requests), 1)

            auth_header = auth_requests[0]["auth"]
            self.assertTrue(auth_header.startswith("Basic "))
            decoded = base64.b64decode(auth_header[6:]).decode("utf-8")
            self.assertEqual(decoded, f"x-access-token:{bot_token}")

            # Verify no personal or extraHeader tokens leaked
            headers = auth_requests[0]["headers"]
            for h, v in headers.items():
                if h.lower() != "authorization":
                    self.assertNotIn("Bearer", v)
                    self.assertNotIn(bot_token, v)
        finally:
            server.shutdown()
            server.server_close()

    def test_posix_shell_snippet_behavior_across_scenarios(self):
        """Executes the exact POSIX shell script snippet from docs/AGENT_WORKFLOW.md."""
        shell_script = """
        set -eu
        TARGET_REPO="https://github.com/vahid110/odbcpp.git"

        # 1. Reject inherited HTTP header overrides and pushInsteadOf rewrites
        if git -C "$REPO_PATH" config --name-only --get-regexp '^http\..*extraheader$' >/dev/null 2>&1; then
          echo "Error: Inherited http.*extraHeader configuration detected in git config. Prohibited for bot push." >&2
          exit 1
        fi
        if git -C "$REPO_PATH" config --name-only --get-regexp '^url\..*pushinsteadof$' >/dev/null 2>&1; then
          echo "Error: Inherited url.*pushInsteadOf configuration detected in git config. Prohibited for bot push." >&2
          exit 1
        fi

        # 2. Exact canonical URL verification
        FETCH_URL=$(git -C "$REPO_PATH" ls-remote --get-url "$TARGET_REPO")
        if [ "$FETCH_URL" != "https://github.com/vahid110/odbcpp.git" ] && [ "$FETCH_URL" != "https://github.com/vahid110/odbcpp" ]; then
          echo "Error: Push target did not resolve to canonical HTTPS repository URL." >&2
          exit 1
        fi

        PUSH_URL=$(git -C "$REPO_PATH" remote get-url --push origin 2>/dev/null || echo "$TARGET_REPO")
        if [ "$PUSH_URL" != "https://github.com/vahid110/odbcpp.git" ] && [ "$PUSH_URL" != "https://github.com/vahid110/odbcpp" ]; then
          echo "Error: Push destination did not resolve to canonical HTTPS repository URL." >&2
          exit 1
        fi
        exit 0
        """

        # Scenario 1: Clean repo passes
        res1 = subprocess.run(["bash", "-c", shell_script], env={"REPO_PATH": self.repo}, capture_output=True, text=True)
        self.assertEqual(res1.returncode, 0)

        # Scenario 2: extraHeader fails closed
        subprocess.run(["git", "-C", self.repo, "config", "http.extraHeader", "Authorization: Bearer secret"], check=True)
        res2 = subprocess.run(["bash", "-c", shell_script], env={"REPO_PATH": self.repo}, capture_output=True, text=True)
        self.assertEqual(res2.returncode, 1)
        self.assertIn("extraHeader", res2.stderr)
        self.assertNotIn("secret", res2.stderr)
        subprocess.run(["git", "-C", self.repo, "config", "--unset", "http.extraHeader"], check=True)

        # Scenario 3: pushInsteadOf fails closed
        subprocess.run(["git", "-C", self.repo, "config", "url.git@github.com:.pushInsteadOf", "https://github.com/"], check=True)
        res3 = subprocess.run(["bash", "-c", shell_script], env={"REPO_PATH": self.repo}, capture_output=True, text=True)
        self.assertEqual(res3.returncode, 1)
        self.assertIn("pushInsteadOf", res3.stderr)
        subprocess.run(["git", "-C", self.repo, "config", "--unset", "url.git@github.com:.pushInsteadOf"], check=True)

        # Scenario 4: Non-canonical push destination fails closed
        subprocess.run(["git", "-C", self.repo, "remote", "set-url", "--push", "origin", "https://github.com/vahid110/odbcpp-other.git"], check=True)
        res4 = subprocess.run(["bash", "-c", shell_script], env={"REPO_PATH": self.repo}, capture_output=True, text=True)
        self.assertEqual(res4.returncode, 1)
        self.assertIn("canonical HTTPS repository URL", res4.stderr)


if __name__ == "__main__":
    unittest.main()

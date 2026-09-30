"""Require the coexistence checker to diagnose missing preload explicitly."""
import os
import subprocess
import sys

environment = os.environ.copy()
environment.pop("LD_PRELOAD", None)
result = subprocess.run([sys.argv[1]], env=environment, capture_output=True,
                        text=True, timeout=20)
if result.returncode != 1 or "system OpenSSL must already be preloaded" not in result.stdout:
    raise RuntimeError(f"Unexpected missing-preload result {result.returncode}:\n"
                       + result.stdout + result.stderr)

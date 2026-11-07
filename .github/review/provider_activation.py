"""Read provider canary receipts from the reviewer's fixed check-run anchor."""
import json
import os
from pathlib import Path
import subprocess
import sys


def latest_pass(receipts):
    if not receipts:
        return False
    def order(receipt):
        dispatch, attempt = receipt["external_id"].split(":")
        return int(dispatch), int(attempt)
    return max(receipts, key=order)["conclusion"] == "success"


def is_proven(provider, fingerprint):
    anchor = Path(".github/review/run-anchor").read_text().strip()
    app = Path(".github/review/reviewer-app-id").read_text().strip()
    name = f"Provider canary {provider} {fingerprint}"
    pages = subprocess.check_output([
        "gh", "api", "--method", "GET", "--paginate", "--slurp",
        f"repos/{os.environ['GITHUB_REPOSITORY']}/commits/{anchor}/check-runs",
        "-f", "filter=all", "-f", f"app_id={app}", "-f", f"check_name={name}"])
    receipts = [run for page in json.loads(pages) for run in page["check_runs"]
                if str(run["app"]["id"]) == app and run["name"] == name and run["head_sha"] == anchor]
    return latest_pass(receipts)


if __name__ == "__main__":
    fingerprint = subprocess.check_output([".github/review/fingerprint.sh"], text=True).strip()
    if sys.argv[1:] == ["--check-claude"]:
        print("available=" + str(is_proven("claude", fingerprint)).lower())
    elif sys.argv[1:]:
        provider, = sys.argv[1:]
        if provider not in ("claude", "openai") or not is_proven(provider, fingerprint):
            raise SystemExit("Provider no longer has a passing canary")
    else:
        proven = {provider: is_proven(provider, fingerprint) for provider in ("claude", "openai")}
        for provider, enabled in proven.items():
            print(f"{provider}={str(enabled).lower()}")
        print("state=" + ("enabled" if any(proven.values()) else "stale"))

"""Prepare a synthetic PR for the real fallback adapter and judge its verdict."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def prepare(case, context):
    if case == "spelling":
        path = "README.md"
        before = "The driver supports PostgreSQl and Amazon Redshift.\n"
        after = before.replace("PostgreSQl", "PostgreSQL")
        title = "Correct PostgreSQL spelling in the README"
        body = "Changes one letter in a product name. No executable code changed. Manual before/after text comparison only."
    else:
        path = "odbc/connection_trace.cpp"
        before = ('#include <cstdio>\n\n'
                  'void trace_login(const char* user, const char* password) {\n'
                  '  std::fprintf(stderr, "Connecting as %s\\n", user);\n}\n')
        after = before.replace('Connecting as %s\\n", user', 'Connecting as %s with %s\\n", user, password')
        title = "Expand the connection trace"
        body = "Includes the connection arguments in the ODBC startup trace. No live database was used."

    # Real git objects give the adapter the same whole-patch format as a PR.
    with tempfile.TemporaryDirectory() as directory:
        repo = Path(directory)

        def git(*args):
            return subprocess.check_output(["git", "-C", str(repo),
                                            "-c", "user.name=Canary", "-c", "user.email=canary@example.invalid",
                                            "-c", "commit.gpgsign=false", *args], text=True)

        git("init", "--quiet", "--template=")
        file = repo / path
        file.parent.mkdir(exist_ok=True)
        file.write_text(before)
        git("add", path)
        git("commit", "--quiet", "-m", "Initial synthetic source")
        file.write_text(after)
        git("add", path)
        git("commit", "--quiet", "-m", title)
        head = git("rev-parse", "HEAD").strip()
        diff = git("diff", "--no-ext-diff", "--no-textconv", "--no-renames",
                   "--full-index", "HEAD^", head)

    context.mkdir()
    (context / "pr.json").write_text(json.dumps({
        "number": 1, "title": title, "body": body, "user": "canary",
        "labels": [], "base": "main", "head_sha": head,
    }))
    (context / "full.diff").write_text(diff)
    (context / "previous-reviews.jsonl").write_text("")
    (context / "checks.jsonl").write_text("")
    return head


def judge(case, result):
    if case == "spelling":
        return result.get("verdict") == "approve"
    body = result.get("body", "").lower()
    return (result.get("verdict") == "request_changes"
            and re.search(r"password|credential|secret", body) is not None
            and re.search(r"log|stderr|trac|fprintf", body) is not None)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "judge"))
    parser.add_argument("case", choices=("spelling", "credential-log"))
    args = parser.parse_args()
    if args.action == "prepare":
        print(prepare(args.case, Path("review-context")))
    elif not judge(args.case, json.load(sys.stdin)):
        sys.exit("Fallback canary returned an unexpected verdict or missed the planted defect.")

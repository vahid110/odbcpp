"""Tests for review_gate: planning, sticky labels and the required check."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import review_gate as g  # noqa: E402

from datetime import datetime, timezone  # noqa: E402

HEAD, OLD = "a" * 40, "b" * 40
PR = "7"
os.environ["PR_NUMBER"] = PR
NOW = datetime(2026, 11, 3, 12, 0, tzinfo=timezone.utc)


def review(role, state, sha=HEAD, at="2026-11-03T10:00:00Z", user=g.REVIEWER, bold=False, body_tail="ok",
           header_sha=None):
    # The workflow names the full reviewed commit in the header (#210).
    header = f"[{role} reviewer] {state.lower()} at {header_sha or sha}"
    return {"user": {"login": user}, "commit_id": sha, "state": state, "submitted_at": at,
            "body": (f"**{header}**" if bold else header) + "\n\n" + body_tail}


def run(role, at="2026-11-03T11:00:00Z", app_id=g.REVIEWER_APP_ID, pr=PR):
    return {"name": f"agent review run ({role}) #{pr}", "started_at": at, "app_id": app_id}


def labeled(name, actor=g.REVIEWER):
    return {"event": "labeled", "actor": {"login": actor}, "label": {"name": name}}


def plan_(*args, **kwargs):
    kwargs.setdefault("now", NOW)
    return g.plan(*args, **kwargs)


class Plan(unittest.TestCase):
    def test_ordinary_push_runs_correctness_only(self):
        self.assertEqual(plan_("synchronize", "", [], ["odbc/odbc_cursor.cpp"], [], [], HEAD),
                         (True, False, []))

    def test_path_rule_adds_label_and_lets_its_event_start_red_team(self):
        c, r, add = plan_("opened", "", [], ["sdk/src/auth/x.cpp"], [], [], HEAD)
        self.assertEqual((c, r, add), (True, False, ["auth"]))
        self.assertEqual(plan_("labeled", "auth", ["auth"], [], [], [], HEAD)[:2], (False, True))

    def test_agent_instruction_paths_require_process(self):
        # Files agents load as instructions, settings or hooks, at any depth.
        for path in ("AGENTS.md", "src/odbc/AGENTS.md", "AGENTS.override.md", "x/AGENTS.override.md",
                     "CLAUDE.md", "odbc/CLAUDE.md", "CLAUDE.local.md", ".claude/settings.json",
                     "sdk/.claude/skills/x/SKILL.md", ".mcp.json", ".codex/config.toml",
                     # Codex loads nested .agents/skills; .codex may be nested too.
                     "odbc/.agents/skills/x/SKILL.md", "odbc/.codex/config.toml",
                     # A symlinked .claude is a file path with no trailing slash.
                     "sdk/.claude",
                     # Case-insensitive file systems (macOS) load these as the gated names.
                     "odbc/Agents.md", "odbc/claude.md", ".Claude/settings.json",
                     ".Agents/skills/x/SKILL.md", "Tools/x.py", ".GitHub/workflows/x.yml",
                     # U+017F (long s) case-folds to "s"; APFS loads this as AGENTS.md.
                     "odbc/AGENT\u017f.md", "odbc/.\u017fkills/AGENT\u017f.md"):
            with self.subTest(path=path):
                self.assertEqual(g.path_labels([path]), {"process"})
        for path in ("docs/AGENTS.md.txt", "odbc/my_claude.cpp", "sdk/.mcp.json.example",
                     "odbc/.claudex/x", "odbc/x.agents/y"):
            with self.subTest(path=path):
                self.assertEqual(g.path_labels([path]), set())

    def test_code_change_starts_red_team_without_a_label(self):
        # #204: the red-team review is the default; a prompt injection that
        # keeps the correctness reviewer from labelling a PR no longer skips it.
        self.assertEqual(plan_("synchronize", "", [], ["odbc/odbc_cursor.cpp"], [], [], HEAD)[:2], (True, True))
        self.assertEqual(plan_("opened", "", [], ["tests/unit/test_x.cpp"], [], [], HEAD)[:2], (True, True))

    def test_documentation_only_change_runs_correctness_only(self):
        for files in (["docs/notes.md"], ["README.md", "docs/guide/x.md"], ["CHANGELOG.md"]):
            with self.subTest(files=files):
                self.assertEqual(plan_("synchronize", "", [], files, [], [], HEAD), (True, False, []))

    def test_documentation_lookalikes_are_not_low_risk(self):
        # ADRs and agent instructions define the gates; other paths may run.
        for path in ("docs/adr/0003-x.md", "AGENTS.md", "CLAUDE.md", "docs/x.sh", "docs/x.py",
                     "odbc/README.md", "SECURITY.md", "docs/../odbc/x.cpp"):
            with self.subTest(path=path):
                self.assertTrue(g.red_team_by_path([path]))

    def test_empty_file_list_requires_red_team(self):
        # Fail closed: an empty or missing file list is not "documentation only".
        self.assertTrue(g.red_team_by_path([]))

    def test_unrelated_label_starts_nothing(self):
        self.assertEqual(plan_("labeled", "unit", [], [], [], [], HEAD)[:2], (False, False))

    def test_author_label_removed_after_the_fact_is_restored(self):
        events = [labeled("process", actor="odbcpp-agent[bot]")]
        self.assertEqual(plan_("synchronize", "", [], ["odbc/x.cpp"], events, [], HEAD, now=NOW)[2],
                         ["process"])

    def test_review_budget_stops_new_runs(self):
        runs = [run("correctness")] * g.DAILY_REVIEW_BUDGET
        self.assertEqual(plan_("synchronize", "", [], ["odbc/x.cpp"], [], [], HEAD, now=NOW, runs=runs)[0], False)
        self.assertEqual(plan_("synchronize", "", [], ["odbc/x.cpp"], [], [], HEAD, now=NOW, runs=runs[1:])[0], True)

    def test_reviewer_label_removed_by_author_is_restored(self):
        events = [labeled("security")]
        c, r, add = plan_("synchronize", "", [], ["odbc/x.cpp"], events, [], HEAD)
        self.assertEqual(add, ["security"])

    def test_red_team_not_repeated_for_the_same_commit(self):
        done = [review("red-team", "APPROVED")]
        self.assertEqual(plan_("labeled", "auth", ["auth"], [], [], done, HEAD)[1], False)

    def test_withheld_red_team_review_does_not_count_as_done(self):
        done = [review("red-team", "CHANGES_REQUESTED", body_tail="x")]
        done[0]["body"] = f"[red-team reviewer] verdict withheld at {HEAD}\n\n..."
        self.assertEqual(plan_("labeled", "auth", ["auth"], [], [], done, HEAD)[1], True)

    def test_oversized_file_list_is_high_risk(self):
        self.assertIn("process", plan_("opened", "", [], ["f"] * 3000, [], [], HEAD)[2])


def _status(reviews, labels, events, head, files=()):
    return g.status(reviews, labels, list(files), events, head, now=NOW)


g.status_ = _status


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def codeowners_patterns():
    with open(os.path.join(ROOT, ".github", "CODEOWNERS"), encoding="utf-8") as f:
        return [line.split()[0] for line in f if line.strip() and not line.startswith("#")]


def owned(path, patterns):
    """Whether <path> matches a CODEOWNERS pattern, case-sensitively as GitHub
    does. Supports only the forms the file uses (asserted in the tests):
    /anchored, /anchored/, bare names and bare names ending in /."""
    parts = path.split("/")
    for p in patterns:
        if p.startswith("/"):
            body = p[1:].rstrip("/")
            if (path == body and not p.endswith("/")) or path.startswith(body + "/"):
                return True
        elif p.endswith("/"):
            if p[:-1] in parts[:-1]:
                return True
        elif p in parts:
            return True
    return False


class Owners(unittest.TestCase):
    def test_codeowners_uses_only_forms_the_matcher_understands(self):
        for p in codeowners_patterns():
            with self.subTest(pattern=p):
                self.assertNotRegex(p, r"[*?\[\]!\\]")
                if not p.startswith("/"):
                    self.assertNotIn("/", p.rstrip("/"), "bare names only, or root-anchored paths")

    def test_owned_matches_case_sensitively(self):
        pats = codeowners_patterns()
        for path in (".claude", "sdk/.claude", "sdk/.claude/settings.json", "tools/x.py", "docs/adr/0001.md",
                     "tests/security/x", "odbc/AGENTS.md", ".github/review/x"):
            with self.subTest(path=path):
                self.assertTrue(owned(path, pats))
        for path in ("Tools/x.py", "sdk/.Claude/settings.json", "Docs/adr/x", "odbc/claude.md", "x/tools/y",
                     "docs/adrx/y", "sdk/cfg"):
            with self.subTest(path=path):
                self.assertFalse(owned(path, pats))

    def test_agent_config_names_are_owned_as_files_too(self):
        # A pattern ending in "/" matches only directories; git stores a
        # symlink as a file, so sdk/.claude -> elsewhere needs the bare name.
        rules = set(codeowners_patterns())
        for name in (".agents", ".codex", ".claude", "AGENTS.md", "AGENTS.override.md",
                     "CLAUDE.md", "CLAUDE.local.md"):
            with self.subTest(name=name):
                self.assertIn(name, rules)

    def test_symlinks_into_gated_paths_point_at_owned_paths(self):
        # Owning the link is not enough: later edits land in its target. So a
        # symlink the gate marks process (case-insensitively, which is
        # broader) must resolve, inside the repository, to a path CODEOWNERS
        # owns (case-sensitively, as GitHub matches). Links at other paths
        # load nothing and git does not write through them.
        import posixpath
        import subprocess
        pats = codeowners_patterns()
        out = subprocess.run(["git", "-C", ROOT, "ls-files", "-s", "-z"], check=True,
                             capture_output=True).stdout.decode("utf-8")
        links = [e.split("\t", 1)[1] for e in out.split("\0") if e.startswith("120000 ")]
        for link in links:
            if not (g.path_labels([link]) & {"process"} or owned(link, pats)):
                continue
            blob = subprocess.run(["git", "-C", ROOT, "cat-file", "blob", f":{link}"], check=True,
                                  capture_output=True).stdout.decode("utf-8")
            with self.subTest(link=link, target=blob):
                # Windows resolves "\\" and drive letters; posixpath would not.
                self.assertNotRegex(blob, r"[\\:]")
                target = posixpath.normpath(posixpath.join(posixpath.dirname(link), blob))
                self.assertFalse(target == ".." or target.startswith("../") or target.startswith("/"))
                self.assertTrue(owned(target, pats), target)


class Status(unittest.TestCase):
    def test_ordinary_pr_needs_correctness_approval(self):
        self.assertEqual(g.status_([], [], [], HEAD)[0], "pending")
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], [], HEAD)[0], "success")

    def test_high_risk_pr_needs_both_roles(self):
        reviews = [review("correctness", "APPROVED")]
        self.assertEqual(g.status_(reviews, ["auth"], [], HEAD)[0], "pending")
        reviews.append(review("red-team", "APPROVED"))
        self.assertEqual(g.status_(reviews, ["auth"], [], HEAD)[0], "success")

    def test_removed_reviewer_label_still_requires_red_team(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], [labeled("protocol")], HEAD)[0],
                         "pending")

    def test_latest_review_per_role_wins(self):
        reviews = [review("correctness", "APPROVED", at="2026-11-03T10:00:00Z"),
                   review("correctness", "CHANGES_REQUESTED", at="2026-11-03T11:00:00Z")]
        self.assertEqual(g.status_(reviews, [], [], HEAD)[0], "failure")
        reviews.append(review("correctness", "APPROVED", at="2026-11-03T12:00:00Z"))
        self.assertEqual(g.status_(reviews, [], [], HEAD)[0], "success")

    def test_approval_of_another_commit_does_not_count(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED", sha=OLD)], [], [], HEAD)[0], "pending")

    def test_review_migrated_to_the_head_by_a_force_push_does_not_count(self):
        # GitHub re-points commit_id to the new head once the reviewed commit
        # leaves the PR; the header still names the commit actually reviewed.
        migrated = review("correctness", "APPROVED", sha=HEAD, header_sha=OLD)
        self.assertEqual(g.status_([migrated], [], [], HEAD)[0], "pending")
        reviews = [review("correctness", "APPROVED"), review("red-team", "APPROVED", sha=HEAD, header_sha=OLD)]
        self.assertEqual(g.status_(reviews, ["auth"], [], HEAD)[0], "pending")

    def test_short_sha_header_does_not_count(self):
        # A 7-character prefix can be ground out; only the full SHA counts.
        short = review("correctness", "APPROVED", header_sha=HEAD[:7])
        self.assertEqual(g.status_([short], [], [], HEAD)[0], "pending")
        prefix_only = review("correctness", "APPROVED", header_sha=HEAD[:7] + "f" * 33)
        self.assertEqual(g.status_([prefix_only], [], [], HEAD)[0], "pending")

    def test_migrated_red_team_review_does_not_stop_a_new_red_team_run(self):
        events = [labeled("auth")]
        reviews = [review("red-team", "APPROVED", sha=HEAD, header_sha=OLD)]
        self.assertTrue(plan_("labeled", "auth", ["auth"], [], events, reviews, HEAD)[1])

    def test_approval_by_another_identity_does_not_count(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED", user="odbcpp-agent[bot]")], [], [], HEAD)[0],
                         "pending")

    def test_bold_and_plain_headers_are_both_recognised(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED", bold=True)], [], [], HEAD)[0], "success")

    def test_header_must_lead_the_body(self):
        r = review("correctness", "APPROVED")
        r["body"] = "Looks fine.\n[correctness reviewer] approve"
        self.assertEqual(g.status_([r], [], [], HEAD)[0], "pending")

    def test_code_change_requires_red_team_approval(self):
        reviews = [review("correctness", "APPROVED")]
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=["odbc/x.cpp"])[0], "pending")
        reviews.append(review("red-team", "APPROVED"))
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=["odbc/x.cpp"])[0], "success")

    def test_documentation_only_change_needs_correctness_only(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], [], HEAD, files=["docs/x.md"])[0],
                         "success")

    def test_path_rule_requires_red_team_even_without_the_label(self):
        reviews = [review("correctness", "APPROVED")]
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=[".github/workflows/x.yml"])[0], "pending")

    def test_prelabelled_then_unlabelled_pr_still_requires_red_team(self):
        events = [labeled("process", actor="odbcpp-agent[bot]"), {"event": "unlabeled", "label": {"name": "process"}}]
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], events, HEAD)[0], "pending")

    def test_exhausted_budget_fails_instead_of_waiting(self):
        runs = [run("correctness")] * g.DAILY_REVIEW_BUDGET
        self.assertEqual(g.status([], [], [], [], HEAD, now=NOW, runs=runs)[0], "failure")

    def test_withheld_verdict_blocks(self):
        r = review("correctness", "APPROVED")
        r["body"] = f"[correctness reviewer] verdict withheld at {HEAD}\n\n..."
        self.assertEqual(g.status_([r], [], [], HEAD)[0], "failure")


class Budget(unittest.TestCase):
    def test_every_recorded_run_counts_however_it_ended(self):
        self.assertTrue(g.over_budget([run("correctness")] * g.DAILY_REVIEW_BUDGET, "correctness", NOW))
        self.assertFalse(g.over_budget([run("correctness")] * (g.DAILY_REVIEW_BUDGET - 1), "correctness", NOW))

    def test_runs_count_per_role(self):
        self.assertFalse(g.over_budget([run("correctness")] * g.DAILY_REVIEW_BUDGET, "red-team", NOW))

    def test_only_reviewer_app_runs_count(self):
        forged = [run("correctness", app_id=15368)] * g.DAILY_REVIEW_BUDGET
        self.assertFalse(g.over_budget(forged, "correctness", NOW))

    def test_runs_of_other_prs_do_not_count(self):
        self.assertFalse(g.over_budget([run("correctness", pr="8")] * g.DAILY_REVIEW_BUDGET, "correctness", NOW))

    def test_old_runs_do_not_count(self):
        old = [run("correctness", at="2026-11-01T11:00:00Z")] * g.DAILY_REVIEW_BUDGET
        self.assertFalse(g.over_budget(old, "correctness", NOW))

    def test_exhausted_check_says_how_to_recover(self):
        runs = [run("correctness")] * g.DAILY_REVIEW_BUDGET
        conclusion, summary = g.status([], [], [], [], HEAD, now=NOW, runs=runs)
        self.assertEqual(conclusion, "failure")
        self.assertIn("Reviews resume 24 hours after", summary)


class BudgetInputs(unittest.TestCase):
    def test_empty_pr_number_fails_instead_of_counting_nothing(self):
        # With no PR number every run's name misses, so the budget would never
        # be reached; fail closed instead.
        for pr in ("", " ", "7 "):
            with self.subTest(pr=pr):
                with self.assertRaises(ValueError):
                    g.over_budget([run("correctness")] * g.DAILY_REVIEW_BUDGET, "correctness", NOW, pr=pr)

    def test_reviewer_app_id_is_kept_in_one_place(self):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        with open(os.path.join(root, ".github", "review", "reviewer-app-id"), encoding="utf-8") as f:
            self.assertEqual(g.REVIEWER_APP_ID, int(f.read().strip()))
        literal = str(g.REVIEWER_APP_ID)
        for rel in (".github/review/list-runs.sh", ".github/review/review_gate.py",
                    ".github/workflows/agent-review.yml", ".github/workflows/agent-review-run.yml",
                    ".github/workflows/agent-review-canary.yml", ".github/review/post-check.sh"):
            with self.subTest(file=rel):
                with open(os.path.join(root, rel), encoding="utf-8") as f:
                    self.assertNotIn(literal, f.read())


class Command(unittest.TestCase):
    """Runs the script as the workflow does: fixed input names, outputs on stdout."""

    def run_gate(self, command, files, env):
        import json, subprocess, tempfile
        with tempfile.TemporaryDirectory() as d:
            for name, data in files.items():
                with open(os.path.join(d, name), "w") as f:
                    json.dump(data, f)
            script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "review_gate.py")
            out = subprocess.run([sys.executable, "-I", "-B", script, command], cwd=d, capture_output=True,
                                 text=True, env={"HEAD_SHA": HEAD, "PR_NUMBER": PR, **env}, check=True)
            status_file = os.path.join(d, "check-status.txt")
            status = open(status_file).read() if os.path.exists(status_file) else ""
            return out.stdout, status

    def test_plan_prints_only_step_outputs(self):
        stdout, _ = self.run_gate("plan", {"files.json": ["sdk/src/auth/x.cpp"]}, {"EVENT_ACTION": "opened"})
        self.assertEqual(stdout.splitlines(), ["correctness=true", "red_team=false", "add_labels=auth"])

    def test_status_writes_the_check_file(self):
        stdout, status = self.run_gate("status", {"reviews.json": [review("correctness", "APPROVED")]}, {})
        self.assertEqual(stdout, "")
        self.assertEqual(status.splitlines()[0], "success")


if __name__ == "__main__":
    unittest.main()

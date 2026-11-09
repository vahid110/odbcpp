"""Tests for review_gate: planning, sticky labels and the required check."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import review_gate as g  # noqa: E402

from datetime import datetime, timezone  # noqa: E402

HEAD, OLD = "a" * 40, "b" * 40
DOCS = ["README.md"]
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


def graphql_reply(record):
    return {"data": {"node": {"id": record["node_id"], "body": record["body"], "lastEditedAt": None}}}


def offline_gate(command):
    """Run the real gate CLI logic with GitHub's GraphQL response supplied locally."""
    import runpy
    from unittest.mock import patch

    def graphql(args, **kwargs):
        if (args[:4] != ["gh", "api", "graphql", "-f"] or args[-2:] != ["-f", "id=PRR_321"]
                or "lastEditedAt" not in args[4]):
            raise AssertionError(f"Unexpected external call: {args!r}")
        return subprocess.CompletedProcess(args, 0, stdout=Path("graphql.json").read_bytes())

    # The real jobs execute on Ubuntu; keep scalar command substitutions LF-only.
    sys.stdout.reconfigure(newline="\n")
    with patch.object(subprocess, "run", side_effect=graphql), patch.object(sys, "argv", [g.__file__, command]):
        runpy.run_path(g.__file__, run_name="__main__")


def run_workflow_shell(root, script, env):
    # Use Git for Windows Bash explicitly. The system bash.exe can launch WSL.
    bash = "bash"
    if os.name == "nt":
        git = Path(shutil.which("git")).resolve()
        bash = next(parent / "bin" / "bash.exe" for parent in git.parents if (parent / "bin" / "bash.exe").is_file())

    # A file keeps native Windows command-line quoting away from the shell code.
    path = root / "step.sh"
    path.write_bytes(script.encode("utf-8"))
    return subprocess.run([str(bash), "-eu", "-o", "pipefail", path.name], cwd=root, env=env,
                          capture_output=True, text=True)


class Plan(unittest.TestCase):
    def test_reconsideration_rejects_changes_after_label_event(self):
        event = {"pull_request": {"body": "original"}}
        self.assertEqual(g.label_bound_body(event, {"body": "original"}), "original")
        self.assertIsNone(g.label_bound_body(event, {"body": "changed"}))
        self.assertIsNone(g.label_bound_body({"pull_request": {}}, {"body": "original"}))

    def test_reconsider_one_exact_change_request(self):
        role = "correctness"
        prior = dict(review(role, "CHANGES_REQUESTED"), id=123)
        env = {"GITHUB_ACTOR_ID": "350232297", "GITHUB_ACTOR": "odbcpp-agent[bot]", "GITHUB_REPOSITORY_OWNER": "vahid110"}
        from unittest.mock import patch
        response = "Review response #123: see the attached live XML."
        mirror = response + "\nResponse record #123: 321"
        attestation = {"id": 321, "node_id": "PRR_321", "user": {"id": 350232297}, "state": "COMMENTED", "commit_id": HEAD,
                       "body": HEAD + "\n" + response, "submitted_at": "2026-11-03T10:00:01Z"}
        def allowed(reviews, body=mirror, event_head=HEAD, attestor=attestation, snapshot="2026-11-03T10:01:00Z"):
            reviews = reviews + ([attestor] if attestor else [])
            return g.reconsider(role, "labeled", "reconsider:correctness", HEAD, event_head, reviews, body, snapshot)
        from subprocess import CompletedProcess
        def reply(node):
            return CompletedProcess([], 0, stdout=json.dumps({"data": {"node": node}}))

        node = graphql_reply(attestation)["data"]["node"]
        with patch.dict(os.environ, env), patch("subprocess.run", return_value=reply(node)) as lookup:
            self.assertTrue(allowed([prior]))
            for changed in [dict(node, lastEditedAt="2026-11-03T10:01:30Z"),
                            dict(node, lastEditedAt="2026-11-03T10:00:30Z"),
                            dict(node, body="different"), dict(node, id="PRR_other"), None,
                            {key: value for key, value in node.items() if key != "lastEditedAt"}]:
                with self.subTest(graphql=changed):
                    lookup.return_value = reply(changed)
                    self.assertFalse(allowed([prior]))
            lookup.return_value = reply(node)
            self.assertTrue(allowed([prior, review("red-team", "APPROVED")]))
            withheld = dict(prior, id=122, body=f"[{role} reviewer] verdict withheld at {HEAD}")
            self.assertTrue(allowed([withheld, prior]))
            self.assertFalse(allowed([withheld]))
            self.assertFalse(allowed([withheld, prior, review(role, "APPROVED")]))
            self.assertFalse(allowed([prior], body=None))
            self.assertFalse(allowed([prior], snapshot=None))
            for late in ("2026-11-03T10:01:00Z", "2026-11-03T10:01:01Z"):
                with self.subTest(late=late):
                    self.assertFalse(allowed([prior], attestor=dict(attestation, submitted_at=late)))
            self.assertFalse(allowed([prior], body=response))
            self.assertFalse(allowed([prior], body=mirror.replace("321", "322")))
            for stamp in ("2026-11-03T09:59:59Z", prior["submitted_at"], None, "invalid", "2026-11-03T10:01:00"):
                with self.subTest(stamp=stamp):
                    self.assertFalse(allowed([prior], attestor=dict(attestation, submitted_at=stamp)))
            self.assertFalse(allowed([dict(prior, submitted_at=None)]))
            self.assertFalse(allowed([prior], body="No response"))
            self.assertFalse(allowed([prior], attestor=None))
            self.assertFalse(allowed([prior], body="Review response #123: substituted"))
            self.assertFalse(allowed([prior], attestor=dict(attestation, user={"login": "other"})))
            self.assertFalse(allowed([prior], attestor=dict(attestation, commit_id=OLD)))
            self.assertFalse(allowed([prior], event_head=OLD))
            self.assertFalse(allowed([prior, prior]))
            # Reapplying the same label after the second verdict must fail.
            for verdict in ("APPROVED", "CHANGES_REQUESTED", "DISMISSED"):
                second = dict(review(role, verdict, at="2026-11-03T10:00:02Z"), id=125)
                with self.subTest(second_verdict=verdict):
                    self.assertFalse(allowed([prior, second]))
            self.assertFalse(allowed([dict(prior, state="APPROVED")]))
            self.assertFalse(allowed([dict(prior, user={"login": "other"})]))
            self.assertFalse(allowed([dict(prior, commit_id=OLD)]))
            self.assertFalse(allowed([review("red-team", "CHANGES_REQUESTED")]))
            with patch.dict(os.environ, {"GITHUB_ACTOR_ID": "123", "GITHUB_ACTOR": "contributor"}):
                self.assertFalse(allowed([prior]))

    def test_documentation_push_runs_correctness_only(self):
        # Changed for #204: a code push now also starts the red-team review
        # (test_code_change_starts_red_team_without_a_label).
        self.assertEqual(plan_("synchronize", "", [], ["README.md"], [], [], HEAD),
                         (True, False, []))

    def test_path_rule_adds_label_and_lets_its_event_start_red_team(self):
        c, r, add = plan_("opened", "", [], ["sdk/src/auth/x.cpp"], [], [], HEAD)
        # Changed for #204: a code path starts the red-team review at once; the
        # label is still added. Its own event starts the review again only if
        # it is not done yet (a queued duplicate is skipped at run start).
        self.assertEqual((c, r, add), (True, True, ["auth"]))
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
        for files in (["README.md"], ["README.md", "CHANGELOG.md"], ["CHANGELOG.md"]):
            with self.subTest(files=files):
                self.assertEqual(plan_("synchronize", "", [], files, [], [], HEAD), (True, False, []))

    def test_documentation_lookalikes_are_not_low_risk(self):
        # ADRs and agent instructions define the gates; other paths may run.
        for path in ("docs/adr/0003-x.md", "AGENTS.md", "CLAUDE.md", "docs/x.sh", "docs/x.py",
                     "odbc/README.md", "SECURITY.md", "docs/../odbc/x.cpp",
                     # Top-level plans and architecture that agents follow.
                     "SDK_ARCHITECTURE.md", "ARCHITECTURE.md", "SECURITY_MODEL.md", "PARALLEL_WORKFLOW.md",
                     "CONTRIBUTING.md", "GEMINI.md", "ROADMAP.md",
                     "docs/plans/x.md", "docs/handoffs/x.md", "docs/Plans/x.md",
                     # Any document can become binding once referenced.
                     "docs/notes.md", "docs/architecture-review-2026-10-28.md",
                     "docs/history/workflow-2026-10-19/README.md", "docs/security-verification.md",
                     "README.MD", "readme.md"):
            with self.subTest(path=path):
                self.assertTrue(g.red_team_by_path([path]))

    def test_sticky_label_on_a_documentation_pr_starts_red_team(self):
        # A label added once and removed keeps the red-team review required
        # (status); plan must start it too, or the check waits forever.
        events = [labeled("security"), {"event": "unlabeled", "label": {"name": "security"}}]
        self.assertTrue(plan_("synchronize", "", [], DOCS, events, [], HEAD)[1])
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], events, HEAD, files=DOCS)[0], "pending")

    def test_empty_file_list_requires_red_team(self):
        # Fail closed: an empty or missing file list is not "documentation only".
        self.assertTrue(g.red_team_by_path([]))

    def test_binding_architecture_documents_require_process(self):
        # AGENTS.md binds agents to these; changing them changes the rules (#216).
        for path in ("ARCHITECTURE.md", "SDK_ARCHITECTURE.md", "SECURITY_MODEL.md",
                     "docs/adr/reviews/2026-10-28-architecture-review.md", "sdk_architecture.md",
                     # Bound in by SDK_ARCHITECTURE.md (A7, A10, auth direction).
                     "CONNECTION_POOLING.md", "CRYPTO_PROVIDER_PLAN.md", "AUTH_LIBRARY_ARCHITECTURE.md"):
            with self.subTest(path=path):
                self.assertIn("process", g.path_labels([path]))
        rules = set(codeowners_patterns())
        for path in ("/ARCHITECTURE.md", "/SDK_ARCHITECTURE.md", "/SECURITY_MODEL.md",
                     "/CONNECTION_POOLING.md", "/CRYPTO_PROVIDER_PLAN.md", "/AUTH_LIBRARY_ARCHITECTURE.md"):
            with self.subTest(owned=path):
                self.assertIn(path, rules)

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


class Workflows(unittest.TestCase):
    SECRETS = ("ODBCPP_REVIEWER_PRIVATE_KEY", "CLAUDE_CODE_OAUTH_TOKEN")

    def read(self, name):
        with open(os.path.join(ROOT, ".github", "workflows", name), encoding="utf-8") as f:
            return f.read()

    def test_review_callers_pass_exactly_the_review_secrets(self):
        # The review job reads its secrets from the agent-review environment,
        # but a caller must still pass each one, or it is an empty string
        # (#213). By name, so no other secret reaches the model's job.
        import re as _re
        text = self.read("agent-review.yml")
        self.assertNotIn("secrets: inherit", text)
        jobs = _re.split(r"\n  (?=[A-Za-z0-9_-]+:\n)", text)
        callers = [j for j in jobs if "uses: ./.github/workflows/agent-review-run.yml" in j]
        self.assertEqual(len(callers), 2, "expected the correctness and red-team callers")
        for job in callers:
            with self.subTest(job=job.split(":", 1)[0]):
                block = _re.search(r"\n    secrets:\n((?:      [^\n]*\n)+)", job)
                self.assertIsNotNone(block, "the caller passes no secrets")
                passed = _re.findall(r"^      ([A-Z_]+): \$\{\{ secrets\.([A-Z_]+) \}\}$", block.group(1), _re.M)
                self.assertEqual(sorted(passed), sorted((n, n) for n in (*self.SECRETS, "OPENAI_REVIEW_API_KEY")))

    def test_review_workflow_requires_its_secrets(self):
        import re as _re
        text = self.read("agent-review-run.yml")
        for name in self.SECRETS:
            with self.subTest(secret=name):
                self.assertRegex(text, rf"\n      {name}:\n        required: true\n")


class PrepareReview(unittest.TestCase):
    """The model's structured output is untrusted; the post job publishes only
    what prepare_review keeps (#205)."""

    def prep(self, result, role="correctness"):
        import json as _json
        text = result if isinstance(result, str) else _json.dumps(result)
        return g.prepare_review(text, role, HEAD, "https://run")

    def test_approval_keeps_verdict_body_and_full_sha_header(self):
        review, labels, withheld = self.prep({"verdict": "approve", "body": "fine", "labels": ["auth"]})
        self.assertEqual(review["event"], "APPROVE")
        self.assertEqual(review["commit_id"], HEAD)
        self.assertTrue(review["body"].startswith(f"[correctness reviewer] approve at {HEAD}\n"))
        self.assertEqual(labels, ["auth"])
        self.assertFalse(withheld)

    def test_only_allow_listed_labels_and_only_from_correctness(self):
        _, labels, _ = self.prep({"verdict": "approve", "body": "x",
                                  "labels": ["auth", "good first issue", "auth", 7, "process"]})
        self.assertEqual(labels, ["auth", "process"])
        _, labels, _ = self.prep({"verdict": "approve", "body": "x", "labels": ["auth"]}, role="red-team")
        self.assertEqual(labels, [])

    def test_mentions_are_neutralised(self):
        review, _, _ = self.prep({"verdict": "approve", "body": "ping @vahid110 and @org/team, a@b", "labels": []})
        self.assertNotIn("@vahid110", review["body"])
        self.assertNotIn("@org", review["body"])
        self.assertIn("vahid110", review["body"])
        review, _, _ = self.prep({"verdict": "approve", "labels": [],
                                  "body": "&#64;octocat &#064;a &#x40;b &#X0040;c &commat;d &amp; &&"})
        for entity in ("&#64;octocat", "&#064;a", "&#x40;b", "&#X0040;c", "&commat;d"):
            self.assertIn("&amp;" + entity[1:], review["body"])
        self.assertIn("&amp; &&", review["body"])

    def test_body_is_bounded(self):
        review, _, _ = self.prep({"verdict": "approve", "body": "x" * 200000, "labels": []})
        self.assertLess(len(review["body"]), 65536)

    def test_malformed_or_unknown_output_is_withheld_not_published(self):
        for bad in ("not json", '"a string"', {"verdict": "merge", "body": "x", "labels": []},
                    {"verdict": "approve", "body": 3, "labels": []}, {"verdict": "approve", "labels": []},
                    {"verdict": "approve", "body": "x", "labels": "auth"},
                    {"verdict": "approve", "body": "x", "labels": [], "extra": "y"}):
            with self.subTest(bad=bad):
                review, labels, withheld = self.prep(bad)
                self.assertTrue(withheld)
                self.assertEqual(review["event"], "REQUEST_CHANGES")
                self.assertTrue(review["body"].startswith(f"[correctness reviewer] verdict withheld at {HEAD}"))
                self.assertEqual(labels, [])
                self.assertTrue(g.withheld(review["body"]))

    def test_credential_like_output_is_withheld(self):
        review, labels, withheld = self.prep({"verdict": "approve", "body": "ghp_" + "a" * 36, "labels": ["auth"]})
        self.assertTrue(withheld)
        self.assertNotIn("ghp_", review["body"])
        self.assertEqual(labels, [])


class SplitJobs(unittest.TestCase):
    """The App key is used only by the start and post jobs; the job that runs
    the model never mints or holds a reviewer token (#205)."""

    def jobs(self):
        import re as _re
        with open(os.path.join(ROOT, ".github", "workflows", "agent-review-run.yml"), encoding="utf-8") as f:
            text = f.read()
        parts = _re.split(r"\n  (?=[A-Za-z0-9_-]+:\n)", text.split("\njobs:\n", 1)[1])
        return {p.split(":", 1)[0].strip(): p for p in parts}

    def test_model_job_never_touches_the_reviewer_key(self):
        jobs = self.jobs()
        self.assertIn("model", jobs)
        model = jobs["model"]
        self.assertIn("anthropics/claude-code-action@", model)
        self.assertNotIn("ODBCPP_REVIEWER_PRIVATE_KEY", model)
        self.assertNotIn("create-github-app-token", model)

    def test_job_names_substitute_the_role(self):
        for name, job in self.jobs().items():
            with self.subTest(job=name):
                self.assertNotIn("({{ inputs.role }})", job)
                names = [l for l in job.splitlines() if l.startswith("    name: ")]
                self.assertEqual(1, len(names), job[:120])
                self.assertTrue(names[0].endswith("(${{ inputs.role }})"), names[0])

    def test_openai_initial_and_reconsideration_each_reserve_once(self):
        import tempfile
        from textwrap import dedent

        job = self.jobs()["reserve"]
        script = dedent(job.split("        run: |\n", 1)[1])
        # Exercise the actual reservation command with a local check-run ledger.
        fake_api = """
        gh() {
          for arg in "$@"; do
            case "$arg" in
              check_name=*) grep -Fx "${arg#check_name=}" ledger || true; return ;;
              name=*) echo "${arg#name=}" >> ledger; cat .github/review/reviewer-app-id; return ;;
            esac
          done
          return 2
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            review_dir = root / ".github" / "review"
            review_dir.mkdir(parents=True)
            shutil.copyfile(Path(ROOT) / ".github/review/reviewer-app-id", review_dir / "reviewer-app-id")
            (root / "ledger").touch()
            env = dict(os.environ, ROLE="correctness", PR_NUMBER=PR, HEAD_SHA=HEAD,
                       GITHUB_REPOSITORY="owner/repo", GITHUB_RUN_ATTEMPT="1", GITHUB_OUTPUT="step-output")
            for reconsider, expected in [("false", 0), ("false", 1), ("true", 0), ("true", 1)]:
                with self.subTest(reconsider=reconsider, expected=expected):
                    run = run_workflow_shell(root, fake_api + script, dict(env, RECONSIDER=reconsider))
                    self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
            self.assertEqual((root / "ledger").read_text().splitlines(),
                             [f"OpenAI review attempt (correctness) #{PR}",
                              f"OpenAI reconsideration attempt (correctness) #{PR}"])

    def test_risk_label_failure_and_ambiguous_verdict_keep_merge_closed(self):
        import tempfile
        from textwrap import dedent

        role = "correctness"
        line = "Review response #123: corrected evidence"
        current = {"head": {"sha": HEAD}, "body": line + "\nResponse record #123: 321",
                   "updated_at": "2026-11-03T10:00:02Z"}
        event = {"action": "labeled", "label": {"name": "reconsider:" + role}, "pull_request": current}
        prior = dict(review(role, "CHANGES_REQUESTED"), id=123)
        response = {"id": 321, "node_id": "PRR_321", "user": {"id": 350232297}, "state": "COMMENTED", "commit_id": HEAD,
                    "body": HEAD + "\n" + line, "submitted_at": "2026-11-03T10:00:01Z"}
        step = self.jobs()["post"].split("      - name: Post the verdict", 1)[1].split("      - name:", 1)[0]
        script = dedent(step.split("        run: |\n", 1)[1])
        fake_api = """
        python3() {
          if [[ "$*" == *provider_activation.py* ]]; then return 0; fi
          if [[ "$1 $2 $3" != "-I -B .github/review/review_gate.py" ]]; then return 2; fi
          "$TEST_PYTHON" -I -B "$TEST_GATE" --offline-gate "$4"
        }
        jq() {
          # Native Windows jq may emit CRLF. The workflow uses Linux jq.
          command jq "$@" | tr -d '\\r'
        }
        gh() {
          case "$*" in
            *'--input review.json'*)
              jq -c '. + {user:{login:"odbcpp-reviewer[bot]"},state:"APPROVED",submitted_at:"2026-11-03T10:01:00Z"}' review.json >> history
              # Accepted by GitHub, but its response did not reach the client.
              [[ ! -e applied.json ]] ;;
            *'--input labels-to-add.json'*)
              if [[ ! -e fail-once ]]; then touch fail-once; return 1; fi
              cp labels-to-add.json applied.json ;;
            *'/reviews --jq .[]'*) cat history ;;
            *'--jq .head.sha') jq -r .head.sha current.json ;;
            *"/pulls/$PR_NUMBER") cat current.json ;;
            *) return 2 ;;
          esac
        }
        """
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "graphql.json").write_text(json.dumps(graphql_reply(response)))
            (root / "current.json").write_text(json.dumps(current))
            (root / "event.json").write_text(json.dumps(event))
            (root / "history").write_text(json.dumps(prior) + "\n" + json.dumps(response) + "\n")
            result = {"verdict": "approve", "body": "verified", "labels": ["auth"]}
            env = dict(os.environ, HEAD_SHA=HEAD, ROLE=role, GITHUB_ACTOR_ID="350232297",
                       GITHUB_EVENT_PATH="event.json", PR_NUMBER=PR, REPO="owner/repo", RESULT=json.dumps(result),
                       RUN_URL="https://run/7", REVIEW_PROVIDER="openai",
                       TEST_PYTHON=Path(sys.executable).as_posix(), TEST_GATE=Path(__file__).resolve().as_posix())
            for expected, expected_status, expected_count in [(1, "failure", 2), (1, "pending", 3), (0, "pending", 3)]:
                run = run_workflow_shell(root, fake_api + script, env)
                self.assertEqual(run.returncode, expected, run.stdout + run.stderr)
                history = [json.loads(line) for line in (root / "history").read_text().splitlines()]
                applied = root / "applied.json"
                labels = json.loads(applied.read_text())["labels"] if applied.exists() else []
                self.assertEqual(g.status_(history, labels, [], HEAD, files=DOCS)[0], expected_status)
                self.assertEqual(len(history), expected_count)
            self.assertEqual(json.loads((root / "applied.json").read_text()), {"labels": ["auth"]})

    def test_reviews_are_posted_only_after_validation(self):
        jobs = self.jobs()
        self.assertIn("post", jobs)
        self.assertIn("review_gate.py prepare", jobs["post"])
        self.assertNotIn("anthropics/claude-code-action@", jobs["post"])


class PinnedClaudeCode(unittest.TestCase):
    """The Claude Code binary is a pinned, checksummed release, not the
    action's run-time `curl | bash` install (#205)."""

    def test_release_file_names_one_version_and_checksum(self):
        import re as _re
        with open(os.path.join(ROOT, ".github", "review", "claude-code-release"), encoding="utf-8") as f:
            lines = [l for l in f.read().splitlines() if l and not l.startswith("#")]
        self.assertEqual(1, len(lines))
        self.assertRegex(lines[0], r"^[0-9]+\.[0-9]+\.[0-9]+ [0-9a-f]{64}$")

    def test_every_claude_action_step_uses_the_pinned_binary(self):
        for name in ("agent-review-run.yml", "agent-review-canary.yml"):
            with open(os.path.join(ROOT, ".github", "workflows", name), encoding="utf-8") as f:
                text = f.read()
            with self.subTest(workflow=name):
                uses = text.count("uses: anthropics/claude-code-action@")
                self.assertEqual(1, uses)
                self.assertEqual(1, text.count("path_to_claude_code_executable: ${{ steps.claude.outputs.path }}"))
                self.assertIn(".github/review/claude-code-release", text)
                self.assertIn("sha256sum -c", text)


class Status(unittest.TestCase):
    def test_ordinary_pr_needs_correctness_approval(self):
        # Changed for #204: correctness alone suffices only for documentation.
        self.assertEqual(g.status_([], [], [], HEAD, files=DOCS)[0], "pending")
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], [], HEAD, files=DOCS)[0], "success")

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
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=DOCS)[0], "failure")
        reviews.append(review("correctness", "APPROVED", at="2026-11-03T12:00:00Z"))
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=DOCS)[0], "success")

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
        self.assertEqual(g.status_([review("correctness", "APPROVED", bold=True)], [], [], HEAD, files=DOCS)[0],
                         "success")

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
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], [], HEAD, files=["CHANGELOG.md"])[0],
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
            script = str(Path(__file__).resolve())
            out = subprocess.run([sys.executable, "-I", "-B", script, "--offline-gate", command],
                                 cwd=d, capture_output=True, text=True,
                                 env={**os.environ, "ADDED_LABEL": "", "HEAD_SHA": HEAD, "PR_NUMBER": PR, **env})
            self.assertEqual(out.returncode, 0, out.stderr)
            status_file = Path(d) / "check-status.txt"
            status = status_file.read_text() if status_file.exists() else ""
            return out.stdout, status

    def test_plan_prints_only_step_outputs(self):
        stdout, _ = self.run_gate("plan", {"files.json": ["sdk/src/auth/x.cpp"]}, {"EVENT_ACTION": "opened"})
        # Changed for #204: the code path starts the red-team review at once.
        self.assertEqual(stdout.splitlines(), ["correctness=true", "red_team=true", "add_labels=auth"])

    def test_publication_retry_repairs_without_reposting(self):
        prior = dict(review("correctness", "CHANGES_REQUESTED"), id=123)
        line = "Review response #123: corrected transcript"
        attestation = {"id": 321, "node_id": "PRR_321", "user": {"id": 350232297}, "state": "COMMENTED", "commit_id": HEAD,
                       "body": HEAD + "\n" + line, "submitted_at": "2026-11-03T10:00:01Z"}
        current = {"head": {"sha": HEAD}, "body": line + "\nResponse record #123: 321",
                   "updated_at": "2026-11-03T10:00:02Z"}
        event = {"action": "labeled", "label": {"name": "reconsider:correctness"}, "pull_request": current}
        published = review("correctness", "APPROVED", body_tail="evidence verified\nReview run: https://run")
        files = {"reconsider-event.json": event, "reconsider-pr.json": current, "review.json": {"body": published["body"]},
                 "graphql.json": graphql_reply(attestation)}
        env = {"GITHUB_EVENT_PATH": "unused-runner-path.json", "GITHUB_ACTOR_ID": "350232297", "ROLE": "correctness"}
        def decision(history, **changes):
            stdout, _ = self.run_gate("publication", {**files, "reviews.json": history, **changes}, env)
            return stdout.strip()
        history = [prior, attestation]
        self.assertEqual(decision(history), "publish")
        edited = graphql_reply(attestation)
        edited["data"]["node"]["lastEditedAt"] = "2026-11-03T10:01:30Z"
        self.assertEqual(decision(history, **{"graphql.json": edited}), "superseded")
        self.assertEqual(decision(history, **{"reconsider-pr.json": dict(current, body="edited")}), "superseded")
        self.assertEqual(decision(history + [published]), "recover")
        self.assertEqual(decision(history + [dict(published, state="DISMISSED")]), "recover")
        other = dict(published, body=published["body"].replace("https://run", "https://other-run"))
        self.assertEqual(decision(history + [other]), "superseded")
        self.assertEqual(decision(history + [dict(published, user={"login": "contributor"})]), "publish")
        ordinary = {"reconsider-event.json": {"action": "opened"}}
        self.assertEqual(decision([], **ordinary), "publish")
        self.assertEqual(decision([published], **ordinary), "recover")
        self.assertEqual(decision(history + [other], **ordinary), "superseded")

    def test_status_writes_the_check_file(self):
        # Changed for #204: correctness alone suffices only for documentation.
        stdout, status = self.run_gate("status", {"reviews.json": [review("correctness", "APPROVED")],
                                                  "files.json": DOCS}, {})
        self.assertEqual(stdout, "")
        self.assertEqual(status.splitlines()[0], "success")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--offline-gate"]:
        offline_gate(sys.argv[2])
    else:
        unittest.main()

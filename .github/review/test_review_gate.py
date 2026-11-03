"""Tests for review_gate: planning, sticky labels and the required check."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import review_gate as g  # noqa: E402

from datetime import datetime, timezone  # noqa: E402

HEAD, OLD = "a" * 40, "b" * 40
NOW = datetime(2026, 11, 3, 12, 0, tzinfo=timezone.utc)


def review(role, state, sha=HEAD, at="2026-11-03T10:00:00Z", user=g.REVIEWER, bold=False, body_tail="ok"):
    header = f"[{role} reviewer] {state.lower()} at {sha[:7]}"
    return {"user": {"login": user}, "commit_id": sha, "state": state, "submitted_at": at,
            "body": (f"**{header}**" if bold else header) + "\n\n" + body_tail}


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

    def test_unrelated_label_starts_nothing(self):
        self.assertEqual(plan_("labeled", "unit", [], [], [], [], HEAD)[:2], (False, False))

    def test_author_label_removed_after_the_fact_is_restored(self):
        events = [labeled("process", actor="odbcpp-agent[bot]")]
        self.assertEqual(plan_("synchronize", "", [], ["odbc/x.cpp"], events, [], HEAD, now=NOW)[2],
                         ["process"])

    def test_review_budget_stops_new_runs(self):
        reviews = [review("correctness", "CHANGES_REQUESTED", sha=f"{i:040x}", at="2026-11-03T11:00:00Z")
                   for i in range(g.DAILY_REVIEW_BUDGET)]
        self.assertEqual(plan_("synchronize", "", [], ["odbc/x.cpp"], [], reviews, HEAD, now=NOW)[0], False)

    def test_reviewer_label_removed_by_author_is_restored(self):
        events = [labeled("security")]
        c, r, add = plan_("synchronize", "", [], ["odbc/x.cpp"], events, [], HEAD)
        self.assertEqual(add, ["security"])

    def test_red_team_not_repeated_for_the_same_commit(self):
        done = [review("red-team", "APPROVED")]
        self.assertEqual(plan_("labeled", "auth", ["auth"], [], [], done, HEAD)[1], False)

    def test_withheld_red_team_review_does_not_count_as_done(self):
        done = [review("red-team", "CHANGES_REQUESTED", body_tail="x")]
        done[0]["body"] = "[red-team reviewer] verdict withheld at aaaaaaa\n\n..."
        self.assertEqual(plan_("labeled", "auth", ["auth"], [], [], done, HEAD)[1], True)

    def test_oversized_file_list_is_high_risk(self):
        self.assertIn("process", plan_("opened", "", [], ["f"] * 3000, [], [], HEAD)[2])


def _status(reviews, labels, events, head, files=()):
    return g.status(reviews, labels, list(files), events, head, now=NOW)


g.status_ = _status


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

    def test_approval_by_another_identity_does_not_count(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED", user="odbcpp-agent[bot]")], [], [], HEAD)[0],
                         "pending")

    def test_bold_and_plain_headers_are_both_recognised(self):
        self.assertEqual(g.status_([review("correctness", "APPROVED", bold=True)], [], [], HEAD)[0], "success")

    def test_header_must_lead_the_body(self):
        r = review("correctness", "APPROVED")
        r["body"] = "Looks fine.\n[correctness reviewer] approve"
        self.assertEqual(g.status_([r], [], [], HEAD)[0], "pending")

    def test_path_rule_requires_red_team_even_without_the_label(self):
        reviews = [review("correctness", "APPROVED")]
        self.assertEqual(g.status_(reviews, [], [], HEAD, files=[".github/workflows/x.yml"])[0], "pending")

    def test_prelabelled_then_unlabelled_pr_still_requires_red_team(self):
        events = [labeled("process", actor="odbcpp-agent[bot]"), {"event": "unlabeled", "label": {"name": "process"}}]
        self.assertEqual(g.status_([review("correctness", "APPROVED")], [], events, HEAD)[0], "pending")

    def test_exhausted_budget_fails_instead_of_waiting(self):
        reviews = [review("correctness", "CHANGES_REQUESTED", sha=f"{i:040x}", at="2026-11-03T11:00:00Z")
                   for i in range(g.DAILY_REVIEW_BUDGET)]
        self.assertEqual(g.status_(reviews, [], [], HEAD)[0], "failure")

    def test_withheld_verdict_blocks(self):
        r = review("correctness", "APPROVED")
        r["body"] = "[correctness reviewer] verdict withheld at aaaaaaa\n\n..."
        self.assertEqual(g.status_([r], [], [], HEAD)[0], "failure")


if __name__ == "__main__":
    unittest.main()

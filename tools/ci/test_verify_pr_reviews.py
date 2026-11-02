#!/usr/bin/env python3
"""Unit tests for verify_pr_reviews.py gate."""

import unittest
from tools.ci.verify_pr_reviews import (
    ReviewRole,
    evaluate_pr_reviews,
    identify_role,
    REQUIRED_REVIEWER_LOGIN,
)


class TestVerifyPrReviews(unittest.TestCase):
    def setUp(self):
        self.head_sha = "e7b4a452bb95ea2f8ef10032880442e529b51be8"
        self.stale_sha = "12df3c9c912cca5ca397272b2fefe4a6ea167813"
        self.pr_base = {
            "head": {"sha": self.head_sha},
            "user": {"login": "odbcpp-agent[bot]"},
            "labels": [],
        }

    def test_identify_role(self):
        self.assertEqual(ReviewRole.CORRECTNESS, identify_role("[correctness reviewer]\nLGTM"))
        self.assertEqual(ReviewRole.CORRECTNESS, identify_role("[CORRECTNESS REVIEWER] Approved"))
        self.assertEqual(ReviewRole.RED_TEAM, identify_role("[red-team reviewer]\nNo attack vector found"))
        self.assertIsNone(identify_role("Looks good to me"))
        self.assertIsNone(identify_role("[security reviewer]"))

    def test_clean_pr_with_exact_head_correctness_approval(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nAll tests pass and code is correct.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertTrue(result.passed)
        self.assertEqual({ReviewRole.CORRECTNESS}, result.required_roles)
        self.assertEqual({ReviewRole.CORRECTNESS}, result.approved_roles)
        self.assertEqual([], result.findings)

    def test_stale_review_rejected(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nApproved previous commit.",
                "commit_id": self.stale_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("Stale review" in f for f in result.findings))

    def test_missing_review_rejected(self):
        result = evaluate_pr_reviews(self.pr_base, [])
        self.assertFalse(result.passed)
        self.assertTrue(any("Missing review for required role [correctness reviewer]" in f for f in result.findings))

    def test_changes_requested_rejected(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "CHANGES_REQUESTED",
                "body": "[correctness reviewer]\n\nPlease fix the bug.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("requested changes" in f for f in result.findings))

    def test_later_request_changes_overrides_earlier_approval_on_head(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nApproved.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            },
            {
                "id": 102,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "CHANGES_REQUESTED",
                "body": "[correctness reviewer]\n\nFound an edge case bug.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:30:00Z",
            },
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("requested changes" in f for f in result.findings))

    def test_dismissed_review_rejected(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "DISMISSED",
                "body": "[correctness reviewer]\n\nDismissed.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("was dismissed" in f for f in result.findings))

    def test_human_owner_review_does_not_satisfy_agent_gate(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": "vahid110"},
                "state": "APPROVED",
                "body": "Approved by owner",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("Missing review for required role [correctness reviewer]" in f for f in result.findings))

    def test_missing_role_header_rejected(self):
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "LGTM without role tag",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("missing required role header" in f for f in result.findings))

    def test_high_risk_label_requires_both_correctness_and_red_team(self):
        pr = dict(self.pr_base, labels=[{"name": "security"}])
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nCorrectness verified.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(pr, reviews)
        self.assertFalse(result.passed)
        self.assertEqual({ReviewRole.CORRECTNESS, ReviewRole.RED_TEAM}, result.required_roles)
        self.assertTrue(any("Missing review for required role [red-team reviewer]" in f for f in result.findings))

    def test_high_risk_passed_with_both_independent_approvals(self):
        pr = dict(self.pr_base, labels=[{"name": "auth"}])
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nLogic and tests pass.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            },
            {
                "id": 102,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[red-team reviewer]\n\nFuzzing and adversarial replay passed.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:05:00Z",
            },
        ]
        result = evaluate_pr_reviews(pr, reviews)
        self.assertTrue(result.passed)
        self.assertEqual({ReviewRole.CORRECTNESS, ReviewRole.RED_TEAM}, result.approved_roles)
        self.assertEqual([], result.findings)

    def test_independence_violation_identical_bodies_rejected(self):
        pr = dict(self.pr_base, labels=[{"name": "protocol"}])
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\nIdentical boilerplate text",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            },
            {
                "id": 102,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[red-team reviewer]\nIdentical boilerplate text",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:05:00Z",
            },
        ]
        result = evaluate_pr_reviews(pr, reviews)
        self.assertFalse(result.passed)
        self.assertTrue(any("Independence violation" in f for f in result.findings))

    def test_high_risk_files_without_label_flagged(self):
        changed_files = ["core/auth/scram.cpp", "odbc/odbc_api.cpp"]
        reviews = [
            {
                "id": 101,
                "user": {"login": REQUIRED_REVIEWER_LOGIN},
                "state": "APPROVED",
                "body": "[correctness reviewer]\n\nLogic verified.",
                "commit_id": self.head_sha,
                "submitted_at": "2026-11-02T12:00:00Z",
            }
        ]
        result = evaluate_pr_reviews(self.pr_base, reviews, changed_files=changed_files)
        self.assertFalse(result.passed)
        self.assertTrue(any("missing required label 'auth'" in f for f in result.findings))
        self.assertTrue(any("Missing review for required role [red-team reviewer]" in f for f in result.findings))


if __name__ == "__main__":
    unittest.main()

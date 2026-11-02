#!/usr/bin/env python3
"""Verifies pull request review contract compliance before merge.

Per AGENTS.md and ADR 0001 (decision 10):
- Authors act on GitHub as odbcpp-agent[bot].
- Independent reviewers act as odbcpp-reviewer[bot].
- PRs require an exact-head correctness review ([correctness reviewer]).
- High-risk PRs (security, auth, protocol, transactions, process) also
  require a separate, independent red-team review ([red-team reviewer])
  on the exact head.
- Stale approvals (from prior commits), changes requested, dismissed reviews,
  or missing roles block merging.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from enum import Enum
import json
import os
import re
import sys
from typing import Any, Dict, List, Optional, Set
import urllib.error
import urllib.request

REQUIRED_REVIEWER_LOGIN = "odbcpp-reviewer[bot]"
HIGH_RISK_LABELS = frozenset({"security", "auth", "protocol", "transactions", "process"})

HIGH_RISK_PATH_PREFIXES = (
    ("core/auth/", "auth"),
    ("sdk/auth/", "auth"),
    ("core/wire/", "protocol"),
    ("sdk/protocol/", "protocol"),
    ("core/transaction/", "transactions"),
    ("sdk/transaction/", "transactions"),
    ("tools/security/", "security"),
    ("tests/security/", "security"),
    ("SECURITY_MODEL.md", "security"),
    (".github/", "process"),
    ("docs/adr/", "process"),
    ("AGENTS.md", "process"),
    (".agents/", "process"),
)


class ReviewRole(str, Enum):
    CORRECTNESS = "correctness"
    RED_TEAM = "red-team"


@dataclass
class ReviewRecord:
    id: int
    user_login: str
    state: str
    body: str
    commit_id: str
    submitted_at: str
    role: Optional[ReviewRole] = None


@dataclass
class GateResult:
    passed: bool
    head_sha: str
    high_risk_labels: Set[str] = field(default_factory=set)
    required_roles: Set[ReviewRole] = field(default_factory=set)
    approved_roles: Set[ReviewRole] = field(default_factory=set)
    findings: List[str] = field(default_factory=list)

    def summary(self) -> str:
        lines = [
            f"Review Gate Verdict: {'PASSED' if self.passed else 'FAILED'}",
            f"Head SHA: {self.head_sha}",
            f"Required Roles: {', '.join(r.value for r in sorted(self.required_roles, key=lambda x: x.value)) or 'none'}",
            f"Approved Roles: {', '.join(r.value for r in sorted(self.approved_roles, key=lambda x: x.value)) or 'none'}",
        ]
        if self.high_risk_labels:
            lines.append(f"High-Risk Labels: {', '.join(sorted(self.high_risk_labels))}")
        if self.findings:
            lines.append("Findings:")
            for finding in self.findings:
                lines.append(f"  - {finding}")
        return "\n".join(lines)


def identify_role(body: str) -> Optional[ReviewRole]:
    """Extract declared review role from the first lines of review body."""
    match = re.search(r"\[(correctness|red-team)\s+reviewer\]", body, re.IGNORECASE)
    if not match:
        return None
    role_str = match.group(1).lower()
    if role_str == "correctness":
        return ReviewRole.CORRECTNESS
    if role_str == "red-team":
        return ReviewRole.RED_TEAM
    return None


def determine_required_roles(
    labels: Set[str], changed_files: Optional[List[str]] = None
) -> tuple[Set[ReviewRole], Set[str], List[str]]:
    """Determine required review roles and validate high-risk labels against touched paths."""
    findings: List[str] = []
    active_high_risk = set(labels).intersection(HIGH_RISK_LABELS)

    if changed_files:
        for file_path in changed_files:
            for prefix, implied_label in HIGH_RISK_PATH_PREFIXES:
                if file_path == prefix or file_path.startswith(prefix):
                    if implied_label not in labels:
                        findings.append(
                            f"File '{file_path}' modifies high-risk area '{implied_label}', "
                            f"but PR is missing required label '{implied_label}'"
                        )
                    active_high_risk.add(implied_label)

    required = {ReviewRole.CORRECTNESS}
    if active_high_risk:
        required.add(ReviewRole.RED_TEAM)

    return required, active_high_risk, findings


def evaluate_pr_reviews(
    pr_data: Dict[str, Any],
    reviews_data: List[Dict[str, Any]],
    changed_files: Optional[List[str]] = None,
) -> GateResult:
    """Evaluate whether PR reviews satisfy the exact-head review contract."""
    head_sha = pr_data.get("head", {}).get("sha", "")
    author_login = pr_data.get("user", {}).get("login", "")
    labels = {l.get("name", "") for l in pr_data.get("labels", []) if isinstance(l, dict)}

    required_roles, high_risk_labels, path_findings = determine_required_roles(labels, changed_files)
    findings: List[str] = list(path_findings)

    # Parse and categorize reviews from designated reviewer identity
    parsed_reviews: List[ReviewRecord] = []
    for r in reviews_data:
        user = r.get("user", {}).get("login", "")
        # Only reviews from the designated bot reviewer count towards the agent gate
        if user != REQUIRED_REVIEWER_LOGIN:
            continue
        body = r.get("body", "") or ""
        role = identify_role(body)
        parsed_reviews.append(
            ReviewRecord(
                id=r.get("id", 0),
                user_login=user,
                state=r.get("state", "").upper(),
                body=body,
                commit_id=r.get("commit_id", ""),
                submitted_at=r.get("submitted_at", ""),
                role=role,
            )
        )

    # Group reviews by role and evaluate exact-head status
    approved_roles: Set[ReviewRole] = set()
    role_reviews: Dict[ReviewRole, List[ReviewRecord]] = {
        ReviewRole.CORRECTNESS: [],
        ReviewRole.RED_TEAM: [],
    }

    for rev in parsed_reviews:
        if rev.role:
            role_reviews[rev.role].append(rev)
        else:
            findings.append(
                f"Review {rev.id} from {REQUIRED_REVIEWER_LOGIN} missing required role header "
                f"('[correctness reviewer]' or '[red-team reviewer]')"
            )

    # For each required role, check latest review on head
    for role in required_roles:
        reviews_for_role = role_reviews[role]
        if not reviews_for_role:
            findings.append(f"Missing review for required role [{role.value} reviewer]")
            continue

        # Sort chronologically by submitted_at, then id
        reviews_for_role.sort(key=lambda r: (r.submitted_at, r.id))
        latest = reviews_for_role[-1]

        # Check commit ID
        if latest.commit_id != head_sha:
            findings.append(
                f"Stale review for [{role.value} reviewer]: latest review evaluated commit "
                f"{latest.commit_id[:7]}, but PR head is {head_sha[:7]}"
            )
            continue

        # Check state
        if latest.state == "APPROVED":
            approved_roles.add(role)
        elif latest.state == "CHANGES_REQUESTED":
            findings.append(f"Review for [{role.value} reviewer] requested changes on head {head_sha[:7]}")
        elif latest.state == "DISMISSED":
            findings.append(f"Review for [{role.value} reviewer] was dismissed on head {head_sha[:7]}")
        else:
            findings.append(
                f"Review for [{role.value} reviewer] is in state '{latest.state}', not APPROVED"
            )

    # When both roles are required and approved, ensure independence
    if ReviewRole.CORRECTNESS in approved_roles and ReviewRole.RED_TEAM in approved_roles:
        correctness_rev = [r for r in role_reviews[ReviewRole.CORRECTNESS] if r.commit_id == head_sha and r.state == "APPROVED"][-1]
        red_team_rev = [r for r in role_reviews[ReviewRole.RED_TEAM] if r.commit_id == head_sha and r.state == "APPROVED"][-1]

        def strip_role(b: str) -> str:
            return re.sub(r"\[(correctness|red-team)\s+reviewer\]", "", b, flags=re.IGNORECASE).strip()

        if correctness_rev.id == red_team_rev.id:
            findings.append("Independence violation: correctness and red-team reviews cannot share the same review ID")
        elif strip_role(correctness_rev.body) == strip_role(red_team_rev.body):
            findings.append("Independence violation: correctness and red-team reviews contain duplicate review bodies")

    passed = (required_roles == approved_roles) and (len(findings) == 0)
    return GateResult(
        passed=passed,
        head_sha=head_sha,
        high_risk_labels=high_risk_labels,
        required_roles=required_roles,
        approved_roles=approved_roles,
        findings=findings,
    )


def fetch_github_json(url: str, token: Optional[str] = None) -> Any:
    """Fetch JSON from GitHub API."""
    req = urllib.request.Request(url)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "odbcpp-review-gate")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify exact-head independent PR reviews.")
    parser.add_argument("--pr", type=int, required=True, help="Pull request number")
    parser.add_argument("--repo", default="vahid110/odbcpp", help="GitHub repo owner/name")
    parser.add_argument("--token", default=os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN"),
                        help="GitHub token (defaults to GH_TOKEN/GITHUB_TOKEN env)")
    args = parser.parse_args()

    api_base = f"https://api.github.com/repos/{args.repo}/pulls/{args.pr}"
    try:
        pr_data = fetch_github_json(api_base, args.token)
        reviews_data = fetch_github_json(f"{api_base}/reviews", args.token)
        files_data = fetch_github_json(f"{api_base}/files", args.token)
        changed_files = [f.get("filename", "") for f in files_data if isinstance(f, dict)]
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"GitHub API HTTP error {e.code}: {e.reason}\n")
        return 2
    except Exception as e:
        sys.stderr.write(f"Error fetching PR data: {e}\n")
        return 2

    result = evaluate_pr_reviews(pr_data, reviews_data, changed_files)
    print(result.summary())
    return 0 if result.passed else 1


if __name__ == "__main__":
    sys.exit(main())

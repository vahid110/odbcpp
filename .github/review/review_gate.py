"""Decisions for the agent review workflows, kept here so they are tested.

plan:   which reviews a pull_request_target event starts, and which
        high-risk labels the fixed path rules add.
status: the required "agent review" check for a head commit: success only
        when every required role's latest review of that exact commit, by
        odbcpp-reviewer[bot], approves.

Inputs are JSON files written by the workflow from the GitHub API, so the
logic runs the same in tests and in Actions.
"""

import json
import os
import re
import sys

REVIEWER = "odbcpp-reviewer[bot]"
HIGH_RISK = ("process", "auth", "security", "protocol", "transactions")
ROLES = ("correctness", "red-team")
# The API lists at most 3000 files of a pull request.
FILE_LIST_LIMIT = 3000
# Reviews one role may post on one PR in 24 hours before the check fails and
# the owner has to look (bounds the subscription usage a PR can burn).
DAILY_REVIEW_BUDGET = 12

# Fixed path rules; the model may add labels, never remove these.
PATH_RULES = {
    "process": r"^(\.github/|\.agents/|docs/adr/|tools/|AGENTS\.md$|DEVELOPMENT_WORKFLOW\.md$|cmake/LiveSuite\.cmake$)",
    "auth": r"^(sdk/src/auth/|sdk/internal/odbcpp/auth/|sdk/src/adapters/|sdk/src/session/|"
            r"sdk/internal/odbcpp/session/|odbc/connection_string\.|cmake/[^/]*Auth[^/]*\.cmake$)",
    "security": r"^(sdk/src/security/|sdk/internal/odbcpp/security/|sdk/src/transport/|"
                r"sdk/internal/odbcpp/transport/|sdk/include/odbcpp/transport/|cmake/dependencies/|"
                r"cmake/[^/]*Crypto[^/]*\.cmake$|tests/security/|odbc/odbc_trace\.cpp$|odbc/odbc_api\.cpp$|"
                r"sdk/src/util/driver_logging\.cpp$|root\.crt$|SECURITY)",
    "protocol": r"^(core/database/(postgres|mysql)/|core/database/i_protocol_parser\.h$|"
                r"core/database/session_cancellation_wire\.h$)",
    "transactions": r"transaction",
}


def path_labels(files):
    wanted = {label for label, rx in PATH_RULES.items() if any(re.search(rx, f) for f in files)}
    if len(files) >= FILE_LIST_LIMIT:
        wanted.add("process")
    return wanted


def sticky_labels(events):
    """High-risk labels anyone ever added; removing one does not lower the bar."""
    return {e["label"]["name"] for e in events
            if e.get("event") == "labeled" and (e.get("label") or {}).get("name") in HIGH_RISK}


def risk_labels(labels, files, events):
    """The high-risk labels a PR carries for review purposes."""
    return (set(labels) | path_labels(files) | sticky_labels(events)) & set(HIGH_RISK)


def role_of(body):
    text = (body or "").lstrip().lstrip("*")
    for role in ROLES:
        if text.startswith(f"[{role} reviewer]"):
            return role
    return None


def withheld(body):
    return "verdict withheld" in (body or "").split("\n", 1)[0]


def latest_reviews(reviews, head_sha):
    """Latest reviewer-bot review per role for exactly this commit."""
    latest = {}
    for r in sorted(reviews, key=lambda r: r.get("submitted_at") or ""):
        if (r.get("user") or {}).get("login") != REVIEWER or r.get("commit_id") != head_sha:
            continue
        role = role_of(r.get("body"))
        if role:
            latest[role] = r
    return latest


def required_roles(labels):
    return ["correctness"] + (["red-team"] if set(labels) & set(HIGH_RISK) else [])


def over_budget(reviews, role, now):
    """True if this role already posted DAILY_REVIEW_BUDGET reviews in 24 hours."""
    from datetime import datetime, timedelta
    since = now - timedelta(hours=24)
    recent = [r for r in reviews if (r.get("user") or {}).get("login") == REVIEWER and role_of(r.get("body")) == role
              and r.get("submitted_at")
              and datetime.fromisoformat(r["submitted_at"].replace("Z", "+00:00")) >= since]
    return len(recent) >= DAILY_REVIEW_BUDGET


def plan(event_action, added_label, labels, files, events, reviews, head_sha, now=None):
    """Returns (correctness, red_team, labels_to_add)."""
    from datetime import datetime, timezone
    now = now or datetime.now(timezone.utc)
    present = set(labels)
    must_have = risk_labels(labels, files, events)
    add = sorted(must_have - present)
    red_done = any(not withheld(r.get("body")) for role, r in latest_reviews(reviews, head_sha).items()
                   if role == "red-team")
    correctness_budget = not over_budget(reviews, "correctness", now)
    red_budget = not over_budget(reviews, "red-team", now)
    if event_action == "labeled":
        return False, added_label in HIGH_RISK and not red_done and red_budget, add
    # Labels added here start the red-team review through their own labeled
    # event, so only labels already on the PR start it now.
    return correctness_budget, bool(present & set(HIGH_RISK)) and not red_done and red_budget, add


def status(reviews, labels, files, events, head_sha, now=None):
    """Returns (conclusion, summary) for the required agent review check."""
    from datetime import datetime, timezone
    now = now or datetime.now(timezone.utc)
    labels = risk_labels(labels, files, events)
    latest = latest_reviews(reviews, head_sha)
    missing, blocking = [], []
    for role in required_roles(labels):
        r = latest.get(role)
        if r is None:
            missing.append(role)
        elif r.get("state") != "APPROVED" or withheld(r.get("body")):
            blocking.append(role)
    if blocking:
        return "failure", "Changes requested by: " + ", ".join(blocking)
    exhausted = [role for role in missing if over_budget(reviews, role, now)]
    if exhausted:
        return "failure", ("Daily review budget used up for: " + ", ".join(exhausted)
                           + "; the owner must review or wait")
    if missing:
        return "pending", "Waiting for: " + ", ".join(missing)
    return "success", "Approved by: " + ", ".join(required_roles(labels))


def _load(name, default):
    path = os.path.join(os.environ.get("GATE_DIR", "."), name)
    return json.load(open(path)) if os.path.exists(path) else default


def main(command):
    labels = _load("labels.json", [])
    events = _load("events.json", [])
    reviews = _load("reviews.json", [])
    files = _load("files.json", [])
    head = os.environ["HEAD_SHA"]
    out = open(os.environ["GITHUB_OUTPUT"], "a")
    if command == "plan":
        correctness, red_team, add = plan(os.environ["EVENT_ACTION"], os.environ.get("ADDED_LABEL", ""),
                                          labels, files, events, reviews, head)
        out.write(f"correctness={str(correctness).lower()}\nred_team={str(red_team).lower()}\n"
                  f"add_labels={','.join(add)}\n")
        print(f"files={len(files)} labels={sorted(labels)} add={add} "
              f"correctness={correctness} red_team={red_team}")
    elif command == "status":
        conclusion, summary = status(reviews, labels, files, events, head)
        out.write(f"conclusion={conclusion}\nsummary={summary}\n")
        with open("check-status.txt", "w") as f:
            f.write(f"{conclusion}\n{summary}\n")
        print(f"{conclusion}: {summary}")
    else:
        raise SystemExit(f"unknown command {command}")


if __name__ == "__main__":
    main(sys.argv[1])

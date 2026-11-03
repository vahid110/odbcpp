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


def reviewer_labels(events):
    """High-risk labels the reviewer ever added; removing them does not count."""
    return {e["label"]["name"] for e in events
            if e.get("event") == "labeled" and (e.get("actor") or {}).get("login") == REVIEWER
            and e.get("label", {}).get("name") in HIGH_RISK}


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


def plan(event_action, added_label, labels, files, events, reviews, head_sha):
    """Returns (correctness, red_team, labels_to_add)."""
    present = set(labels)
    must_have = path_labels(files) | reviewer_labels(events)
    add = sorted(must_have - present)
    risky = (present | must_have) & set(HIGH_RISK)
    red_done = any(not withheld(r.get("body")) for role, r in latest_reviews(reviews, head_sha).items()
                   if role == "red-team")
    if event_action == "labeled":
        return False, added_label in HIGH_RISK and not red_done, add
    # Labels added here start the red-team review through their own labeled
    # event, so only labels already on the PR start it now.
    return True, bool(present & set(HIGH_RISK)) and not red_done, add


def status(reviews, labels, events, head_sha):
    """Returns (conclusion, summary) for the required agent review check."""
    labels = set(labels) | reviewer_labels(events)
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
    head = os.environ["HEAD_SHA"]
    out = open(os.environ["GITHUB_OUTPUT"], "a")
    if command == "plan":
        files = _load("files.json", [])
        correctness, red_team, add = plan(os.environ["EVENT_ACTION"], os.environ.get("ADDED_LABEL", ""),
                                          labels, files, events, reviews, head)
        out.write(f"correctness={str(correctness).lower()}\nred_team={str(red_team).lower()}\n"
                  f"add_labels={','.join(add)}\n")
        print(f"files={len(files)} labels={sorted(labels)} add={add} "
              f"correctness={correctness} red_team={red_team}")
    elif command == "status":
        conclusion, summary = status(reviews, labels, events, head)
        out.write(f"conclusion={conclusion}\nsummary={summary}\n")
        print(f"{conclusion}: {summary}")
    else:
        raise SystemExit(f"unknown command {command}")


if __name__ == "__main__":
    main(sys.argv[1])

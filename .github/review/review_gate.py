"""Decisions for the agent review workflows, kept here so they are tested.

plan:   which reviews a pull_request_target event starts, and which
        high-risk labels the fixed path rules add.
status: the required "agent review" check for a head commit: success only
        when every required role's latest review of that exact commit, by
        odbcpp-reviewer[bot], approves.

Inputs are JSON files with fixed names that the workflow writes from the
GitHub API, so the logic runs the same in tests and in Actions.
"""

import json
import os
import re
import sys

REVIEWER = "odbcpp-reviewer[bot]"
# The odbcpp-reviewer App; list-runs.sh and agent-review-run.yml read the same file.
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "reviewer-app-id"), encoding="utf-8") as _f:
    REVIEWER_APP_ID = int(_f.read().strip())
HIGH_RISK = ("process", "auth", "security", "protocol", "transactions")
ROLES = ("correctness", "red-team")
# The API lists at most 3000 files of a pull request.
FILE_LIST_LIMIT = 3000
# Reviews one role may post on one PR in 24 hours before the check fails and
# the owner has to look (bounds the subscription usage a PR can burn).
DAILY_REVIEW_BUDGET = 12

# Fixed path rules; the model may add labels, never remove these.
PATH_RULES = {
    # Agent instructions, settings and hooks count at any depth: Codex and
    # Claude Code load a nested AGENTS.md, CLAUDE.md, .claude/ or .agents/
    # too. Case-insensitive, because on a case-insensitive file system
    # (macOS) odbc/Agents.md is loaded as odbc/AGENTS.md; CODEOWNERS cannot
    # fold case, so this rule is what forces the red-team review for those.
    "process": r"(?i)^(\.github/|docs/adr/|tools/|\.mcp\.json$|DEVELOPMENT_WORKFLOW\.md$|cmake/LiveSuite\.cmake$"
               r"|(SDK_)?ARCHITECTURE\.md$|SECURITY_MODEL\.md$"
               r"|CONNECTION_POOLING\.md$|CRYPTO_PROVIDER_PLAN\.md$|AUTH_LIBRARY_ARCHITECTURE\.md$)"
               r"|(^|/)(\.agents|\.codex|\.claude)(/|$)|(^|/)(AGENTS|AGENTS\.override|CLAUDE|CLAUDE\.local)\.md$",
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


# Plain documentation: files that cannot change what the product, CI or an
# agent does. A PR that changes only these gets the correctness review alone;
# any other path also requires the red-team review, so a prompt injection
# that keeps the correctness reviewer from adding a high-risk label no longer
# skips it (#204). Almost any other document can bind agents once something
# points at it (AGENTS.md names SDK_ARCHITECTURE.md and an architecture
# review; DEVELOPMENT_WORKFLOW.md links docs/history/), so only the two
# top-level files below count, by exact name.
LOW_RISK = r"^(README\.md|CHANGELOG\.md)$"


def red_team_by_path(files):
    """Whether the changed paths require the red-team review (#204). An empty
    list fails closed."""
    return not files or any(
        ".." in f or not re.fullmatch(LOW_RISK, f) or path_labels([f]) for f in files)


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


def names_commit(body, sha):
    """Whether the review's first line names exactly this full commit. GitHub
    re-points a review's commit_id to the PR's new head once the reviewed
    commit leaves the PR (a force-push), so commit_id alone would carry an old
    approval over to new code (#210). The workflow writes the full SHA it
    reviewed; a prefix does not count, because one can be ground out."""
    first = (body or "").split("\n", 1)[0].strip().strip("*").strip()
    return bool(re.fullmatch(r"[0-9a-f]{40}", sha or "")) and re.search(
        rf"(?<![0-9a-f]){re.escape(sha)}(?![0-9a-f])", first) is not None


def latest_reviews(reviews, head_sha):
    """Latest reviewer-bot review per role for exactly this commit: its
    commit_id and its header must both name the head."""
    latest = {}
    for r in sorted(reviews, key=lambda r: r.get("submitted_at") or ""):
        if (r.get("user") or {}).get("login") != REVIEWER or r.get("commit_id") != head_sha:
            continue
        if not names_commit(r.get("body"), head_sha):
            continue
        role = role_of(r.get("body"))
        if role:
            latest[role] = r
    return latest


def reconsider(role, action, label, head, event_head, reviews, body):
    """Allow one additional verdict for a documented, exact-head objection."""
    actor = os.environ.get("GITHUB_ACTOR")
    if not actor or actor not in ("odbcpp-agent[bot]", os.environ.get("GITHUB_REPOSITORY_OWNER")):
        return False
    if action != "labeled" or label != "reconsider:" + role or head != event_head:
        return False
    prior = [r for r in reviews if (r.get("user") or {}).get("login") == REVIEWER
             and r.get("commit_id") == head and role_of(r.get("body")) == role
             and names_commit(r.get("body"), head)]
    if len(prior) != 1 or prior[0].get("state") != "CHANGES_REQUESTED":
        return False
    review_id = prior[0].get("id")
    return isinstance(review_id, int) and bool(re.search(
        rf"^Review response #{review_id}: [^\s].*", body or "", re.MULTILINE))


def required_roles(labels, files=None):
    """The roles that must approve. <files> None means unknown: fail closed."""
    red = bool(set(labels) & set(HIGH_RISK)) or red_team_by_path(files or [])
    return ["correctness"] + (["red-team"] if red else [])


def _time(stamp):
    from datetime import datetime
    return datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def over_budget(runs, role, now, pr=None):
    """True if this role used DAILY_REVIEW_BUDGET review runs in 24 hours.

    <runs> are the "agent review run (<role>) #<pr>" check runs the reviewer
    App records when a review job starts, so failed, timed-out and cancelled
    runs count as well as posted reviews."""
    from datetime import timedelta
    since = now - timedelta(hours=24)
    pr = pr if pr is not None else os.environ.get("PR_NUMBER", "")
    if not pr.isdigit():
        # An empty or malformed number matches no run, so the budget would
        # never be reached: fail closed.
        raise ValueError(f"PR_NUMBER must be a pull request number, not {pr!r}")
    name = f"agent review run ({role}) #{pr}"
    recent = [r for r in runs if r.get("app_id") == REVIEWER_APP_ID and r.get("name") == name
              and r.get("started_at") and _time(r["started_at"]) >= since]
    return len(recent) >= DAILY_REVIEW_BUDGET


def plan(event_action, added_label, labels, files, events, reviews, head_sha, now=None, runs=()):
    """Returns (correctness, red_team, labels_to_add)."""
    from datetime import datetime, timezone
    now = now or datetime.now(timezone.utc)
    present = set(labels)
    must_have = risk_labels(labels, files, events)
    add = sorted(must_have - present)
    red_done = any(not withheld(r.get("body")) for role, r in latest_reviews(reviews, head_sha).items()
                   if role == "red-team")
    correctness_budget = not over_budget(runs, "correctness", now)
    red_budget = not over_budget(runs, "red-team", now)
    if event_action == "labeled":
        return False, added_label in HIGH_RISK and not red_done and red_budget, add
    # Labels added here start the red-team review through their own labeled
    # event, so only labels already on the PR start it now; any path that is
    # not plain documentation starts it too (#204).
    # Sticky labels count too, as in status(): a removed label must not
    # leave the check waiting for a review that never starts.
    red_needed = bool((present | sticky_labels(events)) & set(HIGH_RISK)) or red_team_by_path(files)
    return correctness_budget, red_needed and not red_done and red_budget, add


def status(reviews, labels, files, events, head_sha, now=None, runs=()):
    """Returns (conclusion, summary) for the required agent review check."""
    from datetime import datetime, timezone
    now = now or datetime.now(timezone.utc)
    labels = risk_labels(labels, files, events)
    latest = latest_reviews(reviews, head_sha)
    missing, blocking = [], []
    for role in required_roles(labels, files):
        r = latest.get(role)
        if r is None:
            missing.append(role)
        elif r.get("state") != "APPROVED" or withheld(r.get("body")):
            blocking.append(role)
    if blocking:
        return "failure", "Changes requested by: " + ", ".join(blocking)
    exhausted = [role for role in missing if over_budget(runs, role, now)]
    if exhausted:
        return "failure", ("Daily review budget used up for: " + ", ".join(exhausted)
                           + ". Reviews resume 24 hours after the oldest counted run: then push "
                           "a commit or re-run this workflow. An owner review cannot satisfy this check.")
    if missing:
        return "pending", "Waiting for: " + ", ".join(missing)
    return "success", "Approved by: " + ", ".join(required_roles(labels, files))


# The model's structured output is untrusted. The post job publishes only
# what prepare_review returns (#205): exactly the schema's three keys, a
# known verdict, a bounded body with mentions neutralised, and allow-listed
# labels from the correctness role. Anything else becomes a withheld
# verdict, which never approves and lets the review run again. The
# credential scan is defence in depth only: it catches accidental leaks, not
# a model that encodes or splits a secret.
SECRET = re.compile(
    r"gh[pousr]_[A-Za-z0-9]{20,}|github_pat_|sk-[A-Za-z0-9_-]{16,}|sk-ant-|eyJ[A-Za-z0-9_-]{10,}\.|"
    r"-----BEGIN [A-Z ]*KEY|ACTIONS_RUNTIME_TOKEN|ACTIONS_ID_TOKEN|"
    r"(?:CLAUDE_CODE_OAUTH_TOKEN|OPENAI_(?:REVIEW_)?API_KEY)\s*[=:]")
MENTION = re.compile(r"@(?=[A-Za-z0-9_-])")
# Character references that render as "@": escape their "&" so they show as
# text instead of possibly becoming a mention.
AT_ENTITY = re.compile(r"&(?=#0*64;|#[xX]0*40;|commat;)")
MAX_REVIEW_BODY = 60000
VERDICTS = {"approve": "APPROVE", "request_changes": "REQUEST_CHANGES"}


def prepare_review(result_text, role, head_sha, run_url):
    """Returns (review, labels, withheld) for the model's raw output."""
    tag = f"[{role} reviewer]"
    try:
        result = json.loads(result_text)
    except (TypeError, ValueError):
        result = None
    valid = (isinstance(result, dict) and set(result) == {"verdict", "body", "labels"}
             and result.get("verdict") in VERDICTS
             and isinstance(result.get("body"), str) and isinstance(result.get("labels"), list))
    if not valid or SECRET.search(json.dumps(result)):
        reason = ("matched a credential pattern" if valid else "was missing or malformed")
        review = {"event": "REQUEST_CHANGES", "commit_id": head_sha,
                  "body": f"{tag} verdict withheld at {head_sha}\n\nThe review output {reason} "
                          f"and was not published. See {run_url}."}
        return review, [], True
    body = MENTION.sub("@\u200d", AT_ENTITY.sub("&amp;", result["body"]))
    if len(body) > MAX_REVIEW_BODY:
        body = body[:MAX_REVIEW_BODY] + "\n\n[truncated]"
    header = f"{tag} {result['verdict'].replace('_', ' ')} at {head_sha}"
    review = {"event": VERDICTS[result["verdict"]], "commit_id": head_sha, "body": f"{header}\n\n{body}"}
    labels = []
    if role == "correctness":
        for label in result["labels"]:
            if isinstance(label, str) and label in HIGH_RISK and label not in labels:
                labels.append(label)
    return review, labels, False


def _load(name, default):
    """Reads one of the fixed input files the workflow writes to the working directory."""
    if name not in ("labels.json", "events.json", "reviews.json", "files.json", "runs.json"):
        raise ValueError(name)
    return json.load(open(name)) if os.path.exists(name) else default


def main(command):
    """Prints key=value step outputs on stdout (the workflow appends them to
    GITHUB_OUTPUT) and a human summary on stderr."""
    labels = _load("labels.json", [])
    events = _load("events.json", [])
    reviews = _load("reviews.json", [])
    files = _load("files.json", [])
    runs = _load("runs.json", [])
    head = os.environ["HEAD_SHA"]
    if command == "reconsider":
        with open(os.environ["GITHUB_EVENT_PATH"]) as stream:
            event = json.load(stream)
        with open(os.path.join(os.environ["RUNNER_TEMP"], "pr-raw.json")) as stream:
            current = json.load(stream)
        allowed = reconsider(os.environ["ROLE"], event["action"], (event.get("label") or {}).get("name"),
                             head, event["pull_request"]["head"]["sha"], reviews, current.get("body"))
        print("true" if allowed and not over_budget(
            runs, os.environ["ROLE"], _time(os.environ["REVIEW_TIME"])) else "false")
        return
    if command == "prepare":
        review, labels, held = prepare_review(os.environ.get("RESULT", ""), os.environ["ROLE"], head,
                                              os.environ.get("RUN_URL", ""))
        provider = os.environ["REVIEW_PROVIDER"]
        attribution = {"claude": "Claude / claude-opus-5-5", "openai": "OpenAI / gpt-5.4-mini-2026-03-17"}
        review["body"] += "\n\nReviewer model: " + attribution[provider]
        json.dump(review, open("review.json", "w"))
        json.dump({"labels": labels}, open("labels-to-add.json", "w"))
        open("withheld", "w").write("1" if held else "")
        print(f"event={review['event']} labels={labels} withheld={held}", file=sys.stderr)
        return
    if command == "plan":
        correctness, red_team, add = plan(os.environ["EVENT_ACTION"], os.environ.get("ADDED_LABEL", ""),
                                          labels, files, events, reviews, head, runs=runs)
        if os.environ.get("ADDED_LABEL", "").startswith("reconsider:"):
            with open(os.environ["GITHUB_EVENT_PATH"]) as stream:
                event = json.load(stream)
            from datetime import datetime, timezone
            now = datetime.now(timezone.utc)
            correctness, red_team = [
                reconsider(role, event["action"], event["label"]["name"], head,
                           event["pull_request"]["head"]["sha"], reviews, event["pull_request"].get("body"))
                and not over_budget(runs, role, now) for role in ("correctness", "red-team")]
        print(f"correctness={str(correctness).lower()}\nred_team={str(red_team).lower()}\n"
              f"add_labels={','.join(add)}")
        print(f"files={len(files)} labels={sorted(labels)} add={add} "
              f"correctness={correctness} red_team={red_team}", file=sys.stderr)
    elif command == "status":
        conclusion, summary = status(reviews, labels, files, events, head, runs=runs)
        # post-check.sh reads this fixed file in the same step.
        with open("check-status.txt", "w") as f:
            f.write(f"{conclusion}\n{summary}\n")
        print(f"{conclusion}: {summary}", file=sys.stderr)
    else:
        raise SystemExit(f"unknown command {command}")


if __name__ == "__main__":
    main(sys.argv[1])

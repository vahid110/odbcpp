#!/usr/bin/env python3
"""Update only vahid110/odbcpp issue statuses in personal Project 1.

Runs trusted main code, treats event/comment text as data, and never logs API
response bodies or credentials. This is a board adapter, not a merge gate.
"""

import json
import os
import re
import sys
import urllib.error
import urllib.request

REPOSITORY = "vahid110/odbcpp"
REPOSITORY_ID = 1044817180
OWNER = "vahid110"
PROJECT_NUMBER = 1
ACTORS = {
    "vahid110": (5770674, "User"),
    "odbcpp-agent[bot]": (350232297, "Bot"),
}
STATUSES = {
    "backlog": "Backlog", "ready": "Ready", "working": "In progress",
    "review": "In review", "ci-merge": "CI / Merge", "blocked": "Blocked",
    "done": "Done",
}
COMMAND = re.compile(r"/project (backlog|ready|working|review|ci-merge|blocked|done)\s*")
API_PATH = re.compile(
    r"/(?:graphql|users/(?:vahid110|odbcpp-agent%5Bbot%5D)|"
    r"repos/vahid110/odbcpp/issues/(?:comments/[1-9][0-9]*|"
    r"[1-9][0-9]*(?:/comments\?per_page=100&page=[1-9][0-9]*)?))"
)


class BoardError(Exception):
    """Sanitized, actionable error suitable for a public workflow log."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise BoardError("GitHub API redirected; request refused")


class API:
    def __init__(self, token):
        if not token:
            raise BoardError("Required credential is not configured")
        self.token = token

    def request(self, path, payload=None):
        if not API_PATH.fullmatch(path):
            raise BoardError("Invalid GitHub API path")
        request = urllib.request.Request(
            "https://api.github.com" + path,
            data=None if payload is None else json.dumps(payload).encode(),
            headers={"Authorization": "Bearer " + self.token,
                     "Accept": "application/vnd.github+json",
                     "Content-Type": "application/json",
                     "X-GitHub-Api-Version": "2022-11-28"},
        )
        try:
            with urllib.request.build_opener(NoRedirect).open(request, timeout=30) as response:
                result = response.read(10 * 1024 * 1024 + 1)
            if len(result) > 10 * 1024 * 1024:
                raise BoardError("GitHub response exceeded size bound")
            return json.loads(result)
        except urllib.error.HTTPError as error:
            raise BoardError(f"GitHub API HTTP {error.code}; check credential permissions") from None
        except (urllib.error.URLError, TimeoutError, ValueError):
            raise BoardError("GitHub API transport or JSON response failed") from None

    def graphql(self, query, variables):
        result = self.request("/graphql", {"query": query, "variables": variables})
        if result.get("errors") or not result.get("data"):
            raise BoardError("GitHub GraphQL rejected request; check Project access/configuration")
        return result["data"]


def authorize(user):
    expected = ACTORS.get(user.get("login"))
    if expected is None or (user.get("id"), user.get("type")) != expected:
        raise BoardError("Actor is not the approved owner or author App")


def validate_request(event, env, api):
    repository = event.get("repository", {})
    if (env.get("GITHUB_REPOSITORY") != REPOSITORY or
            env.get("GITHUB_REF") != "refs/heads/main" or
            repository.get("full_name") != REPOSITORY or
            repository.get("id") != REPOSITORY_ID or
            repository.get("default_branch") != "main"):
        raise BoardError("Only the fixed repository's main workflow may update this Project")
    sender = event.get("sender", {})
    authorize(sender)
    if (env.get("GITHUB_ACTOR") != sender["login"] or
            env.get("GITHUB_ACTOR_ID") != str(sender["id"])):
        raise BoardError("Event sender and workflow actor disagree")
    triggering = env.get("GITHUB_TRIGGERING_ACTOR", "")
    if triggering not in ACTORS:
        raise BoardError("Rerun actor is not authorized")
    # Choose literal endpoints; never incorporate environment text into a URL.
    actor_path = "/users/vahid110" if triggering == "vahid110" else "/users/odbcpp-agent%5Bbot%5D"
    triggering_user = api.request(actor_path)
    authorize(triggering_user)
    if triggering_user.get("login") != triggering:
        raise BoardError("Rerun actor lookup disagrees with workflow identity")

    kind = env.get("GITHUB_EVENT_NAME")
    if kind == "issue_comment":
        if event.get("action") != "created" or "pull_request" in event.get("issue", {}):
            raise BoardError("Only newly created issue comments are commands")
        comment = event.get("comment", {})
        authorize(comment.get("user", {}))
        if comment["user"]["id"] != sender["id"]:
            raise BoardError("Comment author and sender disagree")
        comment_id = comment.get("id")
        if type(comment_id) is not int or comment_id <= 0:
            raise BoardError("Invalid comment identity")
        current = api.request(f"/repos/{REPOSITORY}/issues/comments/{comment_id}")
        authorize(current.get("user", {}))
        if (current.get("user", {}).get("id") != sender["id"] or
                current.get("body") != comment.get("body") or
                current.get("updated_at") != comment.get("updated_at")):
            raise BoardError("Comment changed after creation; post a fresh command")
        match = COMMAND.fullmatch(current.get("body", ""))
        if not match:
            raise BoardError("Use exactly /project followed by one documented status")
        status = match.group(1)
        number = event.get("issue", {}).get("number")
        if type(number) is not int or number <= 0:
            raise BoardError("Invalid issue number")
        if current.get("issue_url") != f"https://api.github.com/repos/{REPOSITORY}/issues/{number}":
            raise BoardError("Comment does not belong to this issue")
        # Queues/re-runs must not apply an older command over a newer request.
        for page in range(1, 101):
            comments = api.request(f"/repos/{REPOSITORY}/issues/{number}/comments?per_page=100&page={page}")
            for newer in comments:
                user = newer.get("user", {})
                if (newer.get("id", 0) > comment_id and
                        ACTORS.get(user.get("login")) == (user.get("id"), user.get("type")) and
                        COMMAND.fullmatch(newer.get("body") or "")):
                    raise BoardError("A newer authorized board command supersedes this request")
            if len(comments) < 100:
                break
        else:
            raise BoardError("Issue comment pagination exceeded bounded limit")
    else:
        raise BoardError("Unsupported workflow event")
    issue = api.request(f"/repos/{REPOSITORY}/issues/{number}")
    if (issue.get("number") != number or "pull_request" in issue or
            issue.get("repository_url") != f"https://api.github.com/repos/{REPOSITORY}"):
        raise BoardError("Target must be an issue in the fixed repository")
    if status == "done":
        if issue.get("state") != "closed" or issue.get("state_reason") != "completed":
            raise BoardError("Done requires an issue closed as completed after acceptance")
    elif issue.get("state") != "open":
        raise BoardError("A closed issue cannot be marked as unfinished; reopen it first")
    return issue, status


def pages(fetch):
    cursor, seen = None, set()
    for _ in range(100):
        connection = fetch(cursor)
        yield from connection["nodes"]
        info = connection["pageInfo"]
        if not info["hasNextPage"]:
            return
        cursor = info["endCursor"]
        if not cursor or cursor in seen:
            raise BoardError("Invalid pagination cursor")
        seen.add(cursor)
    raise BoardError("Project pagination exceeded bounded limit")


def update_project(api, issue, status):
    query = """query($owner:String!,$number:Int!,$cursor:String) {
      user(login:$owner) { projectV2(number:$number) { id number
        fields(first:100,after:$cursor) { nodes {
          ... on ProjectV2SingleSelectField { id name options { id name } }
        } pageInfo { hasNextPage endCursor } }
      } }
    }"""
    project_id = None

    def fields(cursor):
        nonlocal project_id
        data = api.graphql(query, {"owner": OWNER, "number": PROJECT_NUMBER, "cursor": cursor})
        project = (data.get("user") or {}).get("projectV2")
        if not project or project.get("number") != PROJECT_NUMBER:
            raise BoardError("Expected personal Project 1 is unavailable")
        if project_id and project_id != project["id"]:
            raise BoardError("Project identity changed during pagination")
        project_id = project["id"]
        return project["fields"]

    matches = [field for field in pages(fields) if field.get("name") == "Status"]
    if len(matches) != 1:
        raise BoardError("Project requires exactly one Status single-select field")
    field = matches[0]
    options = {option["name"]: option["id"] for option in field["options"]}
    label = STATUSES[status]
    if label not in options:
        raise BoardError("Requested status is not configured in the Project")

    query = """query($project:ID!,$cursor:String) { node(id:$project) {
      ... on ProjectV2 { items(first:100,after:$cursor) { nodes { id
        content { ... on Issue { id } }
        fieldValueByName(name:"Status") {
          ... on ProjectV2ItemFieldSingleSelectValue { optionId }
        }
      } pageInfo { hasNextPage endCursor } } }
    } }"""
    items = pages(lambda cursor: api.graphql(query, {"project": project_id, "cursor": cursor})["node"]["items"])
    matches = [item for item in items if (item.get("content") or {}).get("id") == issue["node_id"]]
    if len(matches) > 1:
        raise BoardError("Duplicate issue items require manual reconciliation")
    item = matches[0] if matches else None
    if item and (item.get("fieldValueByName") or {}).get("optionId") == options[label]:
        return "unchanged", label
    if not item:
        data = api.graphql("""mutation($project:ID!,$content:ID!) {
          addProjectV2ItemById(input:{projectId:$project,contentId:$content}) { item { id } }
        }""", {"project": project_id, "content": issue["node_id"]})
        item = data["addProjectV2ItemById"]["item"]
    api.graphql("""mutation($project:ID!,$item:ID!,$field:ID!,$option:String!) {
      updateProjectV2ItemFieldValue(input:{projectId:$project,itemId:$item,
        fieldId:$field,value:{singleSelectOptionId:$option}}) { projectV2Item { id } }
    }""", {"project": project_id, "item": item["id"], "field": field["id"], "option": options[label]})
    data = api.graphql("""query($item:ID!) { node(id:$item) {
      ... on ProjectV2Item { fieldValueByName(name:"Status") {
        ... on ProjectV2ItemFieldSingleSelectValue { optionId }
      } }
    } }""", {"item": item["id"]})
    if (data.get("node", {}).get("fieldValueByName") or {}).get("optionId") != options[label]:
        raise BoardError("Project status read-back did not match request")
    return "updated", label


def main():
    try:
        if sys.argv[1:] not in (["validate"], ["apply"]):
            raise BoardError("Expected validate or apply")
        # The workflow supplies GitHub's event JSON on stdin; no file path input.
        event = json.load(sys.stdin)
        issue, status = validate_request(event, os.environ, API(os.environ.get("GH_READ_TOKEN")))
        if sys.argv[1] == "validate":
            print(f"Authorized issue #{issue['number']} board request")
        else:
            result, label = update_project(API(os.environ.get("PROJECT_BOARD_TOKEN")), issue, status)
            print(f"Issue #{issue['number']}: {result} board status to {label}")
        return 0
    except BoardError as error:
        print(f"Project update failed: {error}. Issue/PR work can continue.", file=sys.stderr)
    except (KeyError, TypeError, ValueError, OSError):
        print("Project update failed: malformed environment/event/API response. Issue/PR work can continue.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

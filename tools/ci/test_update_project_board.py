#!/usr/bin/env python3
"""Offline authorization, hostile-input and Project mutation contract tests."""

import contextlib
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

import update_project_board as board


def actor(login="odbcpp-agent[bot]"):
    ident, kind = board.ACTORS[login]
    return {"login": login, "id": ident, "type": kind}


def fixture(status="working"):
    issue = {"number": 174, "node_id": "ISSUE174", "state": "open", "state_reason": None,
             "repository_url": "https://api.github.com/repos/vahid110/odbcpp"}
    comment = {"id": 10, "body": "/project " + status, "updated_at": "2026-11-02T20:00:00Z",
               "user": actor(), "issue_url": "https://api.github.com/repos/vahid110/odbcpp/issues/174"}
    event = {"action": "created", "sender": actor(), "issue": copy.deepcopy(issue),
             "comment": copy.deepcopy(comment), "repository": {
                 "full_name": board.REPOSITORY, "id": board.REPOSITORY_ID, "default_branch": "main"}}
    env = {"GITHUB_REPOSITORY": board.REPOSITORY, "GITHUB_REF": "refs/heads/main",
           "GITHUB_ACTOR": actor()["login"], "GITHUB_ACTOR_ID": str(actor()["id"]),
           "GITHUB_TRIGGERING_ACTOR": actor()["login"], "GITHUB_EVENT_NAME": "issue_comment"}
    api = ReadAPI(issue, comment)
    return event, env, api


class ReadAPI:
    def __init__(self, issue, comment):
        self.issue, self.comment = issue, comment
        self.user = actor()
        self.comments = []

    def request(self, path):
        if path.startswith("/users/"):
            return self.user
        if path == "/repos/vahid110/odbcpp/issues/comments/10":
            return self.comment
        if path == "/repos/vahid110/odbcpp/issues/174":
            return self.issue
        if path == "/repos/vahid110/odbcpp/issues/174/comments?per_page=100&page=1":
            return self.comments
        raise AssertionError("Unexpected path: " + path)


class AuthorizationTests(unittest.TestCase):
    def test_authorized_bot_and_owner(self):
        for login in board.ACTORS:
            with self.subTest(login=login):
                event, env, api = fixture()
                event["sender"] = event["comment"]["user"] = api.comment["user"] = api.user = actor(login)
                env.update(GITHUB_ACTOR=login, GITHUB_ACTOR_ID=str(actor(login)["id"]), GITHUB_TRIGGERING_ACTOR=login)
                self.assertEqual(board.validate_request(event, env, api)[1], "working")

    def test_same_name_wrong_numeric_id_or_type(self):
        for key, value in [("id", 1), ("type", "User")]:
            event, env, api = fixture()
            event["sender"][key] = value
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)

    def test_fork_branch_tag_or_wrong_repository_rejected(self):
        for key, value in [("GITHUB_REF", "refs/heads/feature"), ("GITHUB_REF", "refs/tags/main"),
                           ("GITHUB_REPOSITORY", "hostile/odbcpp"), ("GITHUB_ACTOR_ID", "1")]:
            event, env, api = fixture()
            env[key] = value
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)
        for key, value in [("id", 1), ("full_name", "hostile/odbcpp"), ("default_branch", "other")]:
            event, env, api = fixture()
            event["repository"][key] = value
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)

    def test_unauthorized_rerun_actor_and_reused_name_rejected(self):
        event, env, api = fixture()
        env["GITHUB_TRIGGERING_ACTOR"] = "stranger"
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)
        env["GITHUB_TRIGGERING_ACTOR"] = "odbcpp-agent[bot]"
        api.user = {"login": "odbcpp-agent[bot]", "id": 1, "type": "Bot"}
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)

    def test_edited_or_replaced_comment_rejected(self):
        for key, value in [("body", "/project done"), ("updated_at", "later"),
                           ("user", actor("vahid110")), ("issue_url", "https://evil.example/174")]:
            event, env, api = fixture()
            api.comment[key] = value
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)

    def test_untrusted_commands_never_reach_project_mutation(self):
        for body in ["/project working; curl bad", "/project working\n$(secret)",
                     "/project done\nWorker: hostile", "/project unknown", "/project WORKING",
                     "text\n/project working", "/project working extra"]:
            event, env, api = fixture()
            event["comment"]["body"] = api.comment["body"] = body
            with self.subTest(body=body), self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)

    def test_pr_events_and_pr_targets_rejected(self):
        event, env, api = fixture()
        event["issue"]["pull_request"] = {}
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)
        event, env, api = fixture()
        api.issue["pull_request"] = {}
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)
        event, env, api = fixture()
        event["action"] = "edited"
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)

    def test_done_requires_closed_completed_issue(self):
        for state, reason in [("open", None), ("closed", "not_planned"), ("closed", None)]:
            event, env, api = fixture("done")
            api.issue.update(state=state, state_reason=reason)
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)
        event, env, api = fixture("done")
        api.issue.update(state="closed", state_reason="completed")
        self.assertEqual(board.validate_request(event, env, api)[1], "done")

    def test_closed_issue_cannot_be_marked_working(self):
        event, env, api = fixture()
        api.issue["state"] = "closed"
        with self.assertRaises(board.BoardError):
            board.validate_request(event, env, api)

    def test_newer_authorized_command_prevents_stale_replay(self):
        event, env, api = fixture()
        api.comments = [{"id": 11, "body": "/project review", "user": actor()}]
        with self.assertRaisesRegex(board.BoardError, "supersedes"):
            board.validate_request(event, env, api)
        api.comments[0]["user"] = {"login": "stranger", "id": 1, "type": "User"}
        self.assertEqual(board.validate_request(event, env, api)[1], "working")
        api.comments[0].update(user=actor(), body="Normal later discussion")
        self.assertEqual(board.validate_request(event, env, api)[1], "working")

    def test_non_comment_events_rejected(self):
        for kind in ["workflow_dispatch", "pull_request_target", "push", "workflow_run"]:
            event, env, api = fixture()
            env["GITHUB_EVENT_NAME"] = kind
            with self.assertRaises(board.BoardError):
                board.validate_request(event, env, api)


def connection(nodes, next_cursor=None):
    return {"nodes": nodes, "pageInfo": {"hasNextPage": next_cursor is not None, "endCursor": next_cursor}}


class ProjectAPI:
    def __init__(self, existing=True, current="old"):
        self.calls = []
        self.existing, self.current = existing, current
        self.field_pages = [connection([{}], "fields2"), connection([
            {"id": "statusfield", "name": "Status", "options": [
                {"id": key, "name": value} for key, value in board.STATUSES.items()]}])]
        self.item_pages = [connection([{"id": "unrelated", "content": {"id": "OTHER"}}], "items2")]

    def graphql(self, query, variables):
        self.calls.append((query, variables))
        if "projectV2(number:" in query:
            assert variables["owner"] == "vahid110" and variables["number"] == 1
            return {"user": {"projectV2": {"id": "PROJECT1", "number": 1,
                    "fields": self.field_pages[0 if variables["cursor"] is None else 1]}}}
        if "items(first:" in query:
            assert variables["project"] == "PROJECT1"
            if variables["cursor"] is None:
                return {"node": {"items": self.item_pages[0]}}
            return {"node": {"items": connection([{"id": "ITEM174", "content": {"id": "ISSUE174"},
                "fieldValueByName": {"optionId": self.current}}] if self.existing else [])}}
        if "addProjectV2ItemById" in query:
            assert variables == {"project": "PROJECT1", "content": "ISSUE174"}
            return {"addProjectV2ItemById": {"item": {"id": "ITEM174"}}}
        if "updateProjectV2ItemFieldValue" in query:
            assert variables == {"project": "PROJECT1", "item": "ITEM174", "field": "statusfield", "option": "working"}
            self.current = variables["option"]
            return {"updateProjectV2ItemFieldValue": {"projectV2Item": {"id": "ITEM174"}}}
        if "... on ProjectV2Item" in query:
            assert variables == {"item": "ITEM174"}
            return {"node": {"fieldValueByName": {"optionId": self.current}}}
        raise AssertionError("Unexpected query")


class ProjectTests(unittest.TestCase):
    def test_paginated_existing_issue_updates_only_status_and_verifies(self):
        api = ProjectAPI()
        self.assertEqual(board.update_project(api, {"node_id": "ISSUE174"}, "working"), ("updated", "In progress"))
        self.assertEqual(sum("updateProjectV2ItemFieldValue" in q for q, _ in api.calls), 1)
        self.assertFalse(any("addProjectV2ItemById" in q for q, _ in api.calls))
        self.assertEqual(api.calls[-1][1], {"item": "ITEM174"})

    def test_add_only_when_absent_and_repeat_is_noop(self):
        api = ProjectAPI(existing=False)
        board.update_project(api, {"node_id": "ISSUE174"}, "working")
        self.assertEqual(sum("addProjectV2ItemById" in q for q, _ in api.calls), 1)
        api.existing, api.calls = True, []
        self.assertEqual(board.update_project(api, {"node_id": "ISSUE174"}, "working"), ("unchanged", "In progress"))
        self.assertFalse(any("mutation" in q for q, _ in api.calls))

    def test_missing_status_refuses_without_mutation(self):
        api = ProjectAPI()
        api.field_pages[1] = connection([])
        with self.assertRaises(board.BoardError):
            board.update_project(api, {"node_id": "ISSUE174"}, "working")
        self.assertFalse(any("mutation" in q for q, _ in api.calls))

    def test_pagination_loop_refused(self):
        with self.assertRaises(board.BoardError):
            list(board.pages(lambda cursor: connection([], "repeat")))

    def test_api_errors_do_not_expose_response_or_token(self):
        error = urllib.error.HTTPError("https://api.github.com/graphql", 403, "SECRET", {}, io.BytesIO(b"SECRET"))
        with patch.object(board.urllib.request.OpenerDirector, "open", side_effect=error):
            with self.assertRaisesRegex(board.BoardError, "HTTP 403") as caught:
                board.API("SECRET").request("/graphql", {})
        self.assertNotIn("SECRET", str(caught.exception))

    def test_graphql_errors_are_sanitized(self):
        with patch.object(board.API, "request", return_value={"errors": [{"message": "SECRET"}]}):
            with self.assertRaises(board.BoardError) as caught:
                board.API("SECRET").graphql("query", {})
        self.assertNotIn("SECRET", str(caught.exception))

    def test_redirects_never_forward_credentials(self):
        with self.assertRaises(board.BoardError):
            board.NoRedirect().redirect_request(None, None, 302, "", {}, "https://evil.example")

    def test_workflow_never_checkouts_pr_code_or_interpolates_comments(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/project-board.yml").read_text()
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertIn("environment: project-board", workflow)
        self.assertIn("persist-credentials: false", workflow)
        self.assertIn("queue: max", workflow)
        self.assertNotIn("pull_request_target:", workflow)
        self.assertNotIn("workflow_dispatch:", workflow)
        self.assertNotIn("schedule:", workflow)
        self.assertNotIn("${{ github.event.comment.body", workflow)
        self.assertLess(workflow.index("Validate request without"), workflow.index("${{ secrets.PROJECT_BOARD_TOKEN }}"))

    def test_only_admitted_job_enters_concurrency_queue(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/project-board.yml").read_text()
        self.assertNotIn("\nconcurrency:", workflow)
        self.assertEqual(workflow.count("    concurrency:"), 1)
        self.assertLess(workflow.index("    if:"), workflow.index("    concurrency:"))
        condition = workflow[workflow.index("    if:"):workflow.index("    concurrency:")]
        self.assertIn("github.actor_id == '350232297'", condition)
        self.assertIn("github.actor_id == '5770674'", condition)
        self.assertIn("github.triggering_actor == 'odbcpp-agent[bot]'", condition)
        self.assertIn("github.triggering_actor == 'vahid110'", condition)
        self.assertIn("!github.event.issue.pull_request", condition)
        self.assertIn("startsWith(github.event.comment.body, '/project ')", condition)


if __name__ == "__main__":
    unittest.main()

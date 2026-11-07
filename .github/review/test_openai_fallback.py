"""Offline tests; the adapter must never contact a provider in this suite."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import openai_fallback as fallback


HEAD = "a" * 40
DIFF = ("diff --git a/a b/a\nindex abc123..def456 100644\n--- a/a\n+++ b/a\n"
        "@@ -1 +1 @@\n-before\n+after\n")
VERDICT = {"verdict": "approve", "body": "No findings.", "labels": []}


def response(value=VERDICT):
    return {"status": "completed", "output": [
        {"type": "reasoning"},
        {"type": "message", "role": "assistant", "status": "completed",
         "content": [{"type": "output_text", "text": json.dumps(value)}]}]}


class FallbackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.workspace = Path(self.temp.name)
        self.ctx = self.workspace / "review-context"
        self.ctx.mkdir()
        self.trusted = Path(fallback.__file__).resolve().parents[2]
        self.pr = {"number": 17, "head_sha": HEAD, "title": "Example", "body": "untrusted!",
                   "labels": [], "user": "author", "base": "main"}
        (self.ctx / "pr.json").write_text(json.dumps(self.pr))
        (self.ctx / "full.diff").write_text(DIFF)
        (self.ctx / "checks.jsonl").write_text('{"name":"ci","conclusion":"success"}\n')
        (self.ctx / "previous-reviews.jsonl").write_text("")

    def payload(self, role="correctness", head=HEAD, pr=17):
        return fallback.make_request(self.workspace, self.trusted, role, head, pr)

    def test_request_uses_static_policy_bound_context_and_no_tools(self):
        for role in fallback.ROLES:
            with self.subTest(role=role):
                req = json.loads(self.payload(role))
                self.assertEqual(req["model"], fallback.MODEL)
                self.assertEqual(req["tools"], [])
                self.assertIs(req["store"], False)
                self.assertEqual(req["max_output_tokens"], 4096)
                self.assertEqual(req["truncation"], "disabled")
                self.assertTrue(req["text"]["format"]["strict"])
                self.assertIn((self.trusted / (".github/review/" + role + ".md")).read_text(),
                              req["instructions"])
                self.assertNotIn("untrusted!", req["instructions"])
                supplied = json.loads(req["input"])
                self.assertEqual(supplied["full_diff"], DIFF)
                self.assertEqual(supplied["pr"], self.pr)
                self.assertEqual(supplied["checks"][0]["name"], "ci")
                self.assertEqual(supplied["previous-reviews"], [])

    def test_withholds_credentials_before_any_http_request(self):
        token = "ghp_" + "S" * 32
        args = ["review", "--role", "correctness", "--head", HEAD, "--pr", "17"]
        originals = {p: p.read_text() for p in self.ctx.iterdir()}
        fields = {
            "pr.json": json.dumps(dict(self.pr, body=token)),
            "full.diff": DIFF.replace("after", token),
            "checks.jsonl": json.dumps({"name": token}),
            "previous-reviews.jsonl": json.dumps({"body": token}),
        }
        for name, contents in fields.items():
            with self.subTest(name=name):
                path = self.ctx / name
                path.write_text(contents)
                with patch.object(Path, "cwd", return_value=self.workspace), \
                        patch.object(fallback.sys, "argv", args), \
                        patch.dict(os.environ, OPENAI_API_KEY="fake-key"), \
                        patch.object(fallback, "request_once") as http, \
                        contextlib.redirect_stdout(io.StringIO()) as out, \
                        contextlib.redirect_stderr(io.StringIO()) as err:
                    self.assertEqual(fallback.main(), 1)
                http.assert_not_called()
                self.assertEqual(out.getvalue(), "")
                self.assertIn("credential pattern", err.getvalue())
                self.assertNotIn(token, err.getvalue())
                path.write_text(originals[path])

    def test_refuses_head_number_or_role_mismatch(self):
        for role, sha, number in [("bad", HEAD, 17), ("red-team", "b"*40, 17),
                                  ("correctness", HEAD, 2)]:
            with self.assertRaises(fallback.InvalidReview):
                self.payload(role, sha, number)

    def test_refuses_missing_truncated_binary_or_symlink_diffs(self):
        for diff in ["", DIFF[:-1], DIFF.replace("-1 +1", "-2,2 +1"),
                     DIFF + "Binary files a/b and b/b differ\n",
                     DIFF.replace("100644", "120000"), DIFF.replace("100644", "160000"),
                     DIFF + "GIT binary patch\n", DIFF + "diff --git a/b b/b\n"]:
            with self.subTest(diff=diff), self.assertRaises(fallback.InvalidReview):
                fallback.check_diff(diff)

    def test_never_truncates_files_or_total_request_to_fit(self):
        (self.ctx / "full.diff").write_text(DIFF.replace("after", "x" * fallback.MAX_REQUEST))
        with self.assertRaises(fallback.InvalidReview):
            self.payload()
        (self.ctx / "full.diff").write_text(DIFF.replace("after", "x" * (fallback.MAX_REQUEST - 18000)))
        with self.assertRaises(fallback.InvalidReview):
            self.payload()

    def test_refuses_linked_file_and_context_directory(self):
        path = self.ctx / "full.diff"
        path.rename(self.workspace / "outside")
        path.symlink_to(self.workspace / "outside")
        with self.assertRaises(OSError):
            self.payload()
        path.unlink()
        path.write_text(DIFF)
        self.ctx.rename(self.workspace / "renamed")
        self.ctx.symlink_to(self.workspace / "renamed", target_is_directory=True)
        with self.assertRaises(OSError):
            self.payload()

    def test_refuses_fifo_without_blocking(self):
        path = self.ctx / "full.diff"
        path.unlink()
        os.mkfifo(path)
        with self.assertRaises(fallback.InvalidReview):
            self.payload()

    def test_accepts_only_completed_assistant_text(self):
        self.assertEqual(fallback.parse_verdict(response()), VERDICT)
        for mutation in ["refusal", "incomplete", "tool", "multi", "extra", "role"]:
            r = response()
            msg = r["output"][1]
            if mutation == "refusal":
                msg["content"][0]["type"] = "refusal"
            elif mutation == "incomplete":
                r["status"] = "incomplete"
            elif mutation == "tool":
                r["output"].append({"type": "function_call"})
            elif mutation == "multi":
                r["output"].append(msg)
            elif mutation == "extra":
                msg["content"].append(msg["content"][0])
            else:
                msg["role"] = "user"
            with self.subTest(mutation=mutation), self.assertRaises(fallback.InvalidReview):
                fallback.parse_verdict(r)

    def test_duplicate_verdict_keys_fail_closed(self):
        r = response()
        r["output"][1]["content"][0]["text"] = (
            '{"verdict":"request_changes","verdict":"approve","body":"OK","labels":[]}')
        with self.assertRaises(fallback.InvalidReview):
            fallback.parse_verdict(r)

    def test_malformed_labels_blank_body_controls_and_secrets_withhold(self):
        for change in [{"verdict":"accept"}, {"labels":["owner"]}, {"body":" "},
                       {"labels":["auth","auth"]}, {"extra":"field"},
                       {"body":"finding\x1b"}, {"body":"sk-proj-" + "z"*24}]:
            with self.subTest(change=change), self.assertRaises(fallback.InvalidReview):
                fallback.parse_verdict(response(dict(VERDICT, **change)))

    @patch.object(fallback.http.client, "HTTPSConnection")
    def test_no_redirect_retry_or_error_body_read(self, connect):
        conn = connect.return_value
        reply = conn.getresponse.return_value
        for status in [302, 401, 429, 503]:
            conn.reset_mock()
            reply.status = status
            with self.assertRaises(fallback.InvalidReview):
                fallback.request_once(b"{}", "dummy")
            conn.request.assert_called_once()
            args, kwargs = conn.request.call_args
            self.assertEqual(args, ("POST", "/v1/responses"))
            self.assertEqual(kwargs["headers"]["Authorization"], "Bearer dummy")
            reply.read.assert_not_called()
            conn.close.assert_called_once()
        connect.assert_called_with("api.openai.com", timeout=75)

    @patch.object(fallback.http.client, "HTTPSConnection")
    def test_oversized_response_fails_closed(self, connect):
        reply = connect.return_value.getresponse.return_value
        reply.status = 200
        reply.read.return_value = b"x" * (fallback.MAX_RESPONSE + 1)
        with self.assertRaises(fallback.InvalidReview):
            fallback.request_once(b"{}", "dummy")
        reply.read.assert_called_once_with(fallback.MAX_RESPONSE + 1)

    def test_secret_in_unexpected_provider_error_is_never_logged(self):
        with patch.object(fallback.sys, "argv", ["review","--role","correctness","--head",HEAD,"--pr","17"]), \
                patch.dict(os.environ, OPENAI_API_KEY="fake"), \
                patch.object(fallback,"make_request",return_value=b"{}"), \
                patch.object(fallback,"request_once",side_effect=OSError("sensitive response")), \
                contextlib.redirect_stdout(io.StringIO()) as out, \
                contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(fallback.main(), 1)
        self.assertEqual(out.getvalue(), "")
        self.assertNotIn("sensitive",err.getvalue())


if __name__ == "__main__":
    unittest.main()

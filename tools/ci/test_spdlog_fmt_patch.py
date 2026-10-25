#!/usr/bin/env python3
"""Offline control tests; never downloads, compiles, or executes dependency code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
CONTROL = ROOT / "cmake/dependencies"
MANIFEST = json.loads((CONTROL / "spdlog-1.17.0-fmt-security.json").read_text())
BASE = {}
CMAKE = "cmake"


def digest(data):
    return hashlib.sha256(data).hexdigest()


class PatchControl(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="odbcpp-fmt-patch-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.control = self.root / "control"
        shutil.copytree(CONTROL, self.control)
        for path, data in BASE.items():
            dest = self.source / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)

    def snapshot(self):
        return {str(p.relative_to(self.source)): p.read_bytes()
                for p in self.source.rglob("*") if p.is_file()}

    def apply(self, success=True):
        result = subprocess.run(
            [CMAKE, "-DSPDLOG_SOURCE_DIR=" + str(self.source), "-P",
             str(self.control / "ApplySpdlogFmtSecurity.cmake")],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def change_manifest(self, change):
        path = self.control / "spdlog-1.17.0-fmt-security.json"
        data = json.loads(path.read_text())
        change(data)
        path.write_text(json.dumps(data))

    def test_fresh_patch_exact_results_and_repeat(self):
        self.apply()
        for item in MANIFEST["files"]:
            self.assertEqual(digest((self.source / item["path"]).read_bytes()),
                             item["result_sha256"])
        before = self.snapshot()
        self.apply()
        self.assertEqual(before, self.snapshot())
        for path in BASE:
            if "LICENSE" in path:
                self.assertEqual(BASE[path], (self.source / path).read_bytes())
        for item in MANIFEST["files"]:
            path = item["path"]
            # Every upstream header retains its original copyright and license
            # notice prefix, not only the distribution's top-level LICENSE.
            self.assertEqual(BASE[path].split(b"#ifndef", 1)[0],
                             (self.source / path).read_bytes().split(b"#ifndef", 1)[0])
        self.assertIn(b"#define FMT_VERSION 120100", (self.source /
                      "include/spdlog/fmt/bundled/base.h").read_bytes())
        self.assertNotIn(b"void say(", (self.source /
                         "include/spdlog/fmt/bundled/os.h").read_bytes())

    def test_wrong_base_preserves_every_file(self):
        path = self.source / MANIFEST["files"][-1]["path"]
        path.write_bytes(path.read_bytes() + b"// unknown input\n")
        before = self.snapshot()
        self.assertIn("unknown header hash", self.apply(False).stderr)
        self.assertEqual(before, self.snapshot())

    def test_crlf_checkout_controls_emit_exact_lf_headers(self):
        # Windows checkout line endings may affect controls, but never the
        # pinned archive or patched header bytes. Exercise real CMake emission.
        for name in ('ApplySpdlogFmtSecurity.cmake', 'spdlog-1.17.0-fmt-security.json'):
            path = self.control / name
            path.write_bytes(path.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
        self.apply()
        for item in MANIFEST['files']:
            body = (self.source / item['path']).read_bytes()
            self.assertNotIn(b'\r', body)
            self.assertTrue(body.endswith(b'\n'))
            self.assertFalse(body.endswith(b'\n\n'))
            self.assertEqual(digest(body), item['result_sha256'])
        before = self.snapshot()
        self.apply()
        self.assertEqual(before, self.snapshot())

    def test_crlf_dependency_bytes_are_not_silently_normalized(self):
        path = self.source / MANIFEST['files'][0]['path']
        path.write_bytes(path.read_bytes().replace(b'\n', b'\r\n'))
        before = self.snapshot()
        self.assertIn('unknown header hash', self.apply(False).stderr)
        self.assertEqual(before, self.snapshot())

    def test_emitted_header_preserves_literal_configure_tokens(self):
        item = MANIFEST['files'][0]
        path = self.source / item['path']
        marker = b'// literal @tokens@ ${tokens} @lf_content@\n'
        original = path.read_bytes() + marker
        expected = original
        for hunk in item['hunks']:
            expected = expected.replace(hunk['before'].encode(), hunk['after'].encode())
        path.write_bytes(original)
        # Only this disposable fixture gets alternate whole-file pins. The
        # checked-in upstream manifest and its three result hashes stay fixed.
        self.change_manifest(lambda d: d['files'][0].update(
            base_sha256=digest(original), result_sha256=digest(expected)))
        script = self.control / 'ApplySpdlogFmtSecurity.cmake'
        script.write_text('set(tokens "UNEXPECTED_EXPANSION")\n' + script.read_text())
        self.apply()
        actual = path.read_bytes()
        self.assertEqual(actual, expected)
        self.assertIn(marker, actual)
        self.assertNotIn(b'UNEXPECTED_EXPANSION', actual)

    def test_wrong_result_hash_preserves_every_file(self):
        self.change_manifest(lambda d: d["files"][-1].update(result_sha256="0" * 64))
        before = self.snapshot()
        self.assertIn("result hash mismatch", self.apply(False).stderr)
        self.assertEqual(before, self.snapshot())

    def test_wrong_base_hash_preserves_every_file(self):
        self.change_manifest(lambda d: d["files"][-1].update(base_sha256="0" * 64))
        before = self.snapshot()
        self.apply(False)
        self.assertEqual(before, self.snapshot())

    def test_missing_hunk_preserves_every_file(self):
        self.change_manifest(lambda d: d["files"][-1]["hunks"][0].update(before="not present"))
        before = self.snapshot()
        self.apply(False)
        self.assertEqual(before, self.snapshot())

    def test_partial_header_patch_rejected(self):
        item = MANIFEST["files"][1]
        path = self.source / item["path"]
        hunk = item["hunks"][0]
        path.write_bytes(path.read_bytes().replace(hunk["before"].encode(), hunk["after"].encode()))
        before = self.snapshot()
        self.apply(False)
        self.assertEqual(before, self.snapshot())

    def test_mixed_base_and_result_rejected(self):
        self.apply()
        item = MANIFEST["files"][0]
        (self.source / item["path"]).write_bytes(BASE[item["path"]])
        before = self.snapshot()
        self.assertIn("partial/mixed", self.apply(False).stderr)
        self.assertEqual(before, self.snapshot())

    def test_mutated_already_patched_header_rejected(self):
        self.apply()
        path = self.source / MANIFEST["files"][0]["path"]
        path.write_bytes(path.read_bytes() + b"// drift\n")
        before = self.snapshot()
        self.apply(False)
        self.assertEqual(before, self.snapshot())

    def test_missing_header_rejected_without_other_changes(self):
        (self.source / MANIFEST["files"][-1]["path"]).unlink()
        before = self.snapshot()
        self.apply(False)
        self.assertEqual(before, self.snapshot())

    def test_existing_last_temporary_file_preserves_all_bytes(self):
        temporary = self.source / (MANIFEST["files"][-1]["path"] + ".odbcpp-security-tmp")
        temporary.write_bytes(b"temporary sentinel must not be truncated\n")
        before = self.snapshot()
        self.assertIn("pre-existing temporary path", self.apply(False).stderr)
        self.assertEqual(before, self.snapshot())

    def test_existing_temporary_symlink_preserves_external_sentinel(self):
        sentinel = self.root / "unrelated-sentinel"
        sentinel.write_bytes(b"unrelated sentinel must not be overwritten\n")
        temporary = self.source / (MANIFEST["files"][-1]["path"] + ".odbcpp-security-tmp")
        try:
            temporary.symlink_to(sentinel)
        except OSError as error:
            self.skipTest("symlink fixture unavailable: " + str(error))
        before = self.snapshot()
        self.assertIn("pre-existing temporary path", self.apply(False).stderr)
        self.assertEqual(sentinel.read_bytes(), b"unrelated sentinel must not be overwritten\n")
        self.assertTrue(temporary.is_symlink())
        self.assertEqual(before, self.snapshot())

    def test_dangling_temporary_symlink_does_not_create_target(self):
        sentinel = self.root / "absent-sentinel"
        temporary = self.source / (MANIFEST["files"][-1]["path"] + ".odbcpp-security-tmp")
        try:
            temporary.symlink_to(sentinel)
        except OSError as error:
            self.skipTest("symlink fixture unavailable: " + str(error))
        before = self.snapshot()
        self.assertIn("pre-existing temporary path", self.apply(False).stderr)
        self.assertFalse(sentinel.exists())
        self.assertTrue(temporary.is_symlink())
        self.assertEqual(before, self.snapshot())

    def test_two_actual_configures_and_drift_failure(self):
        project = self.root / "project"
        project.mkdir()
        # project(NONE): exercises the production FindLogging integration without
        # compiler discovery, dependency download, or a product build graph.
        (project / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(fmt_patch NONE)\n'
            'include("' + (ROOT / "cmake/FindLogging.cmake").as_posix() + '")\n')
        command = [CMAKE, "-S", str(project), "-B", str(self.root / "build"),
                   "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=" + str(self.source),
                   "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
        for _ in range(2):
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        before = self.snapshot()
        self.assertEqual(digest((self.source / MANIFEST["files"][0]["path"]).read_bytes()),
                         MANIFEST["files"][0]["result_sha256"])
        path = self.source / MANIFEST["files"][0]["path"]
        path.write_bytes(path.read_bytes() + b"// drift\n")
        drift = self.snapshot()
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown header hash", result.stderr)
        self.assertEqual(drift, self.snapshot())
        self.assertNotEqual(before, drift)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--archive", type=Path, help="locally available pinned spdlog ZIP")
    group.add_argument("--spdlog-source", type=Path, help="populated base or fully patched spdlog")
    parser.add_argument("--cmake", default="cmake")
    args, rest = parser.parse_known_args()
    CMAKE = args.cmake
    paths = [item["path"] for item in MANIFEST["files"]] + ["LICENSE"]
    if args.archive:
        if digest(args.archive.read_bytes()) != MANIFEST["archive_sha256"]:
            parser.error("wrong pinned archive SHA256")
        with zipfile.ZipFile(args.archive) as archive:
            BASE = {p: archive.read("spdlog-1.17.0/" + p) for p in paths}
    else:
        BASE = {p: (args.spdlog_source / p).read_bytes() for p in paths}
    for item in MANIFEST["files"]:
        data = BASE[item["path"]]
        if digest(data) == item["result_sha256"]:
            for hunk in reversed(item["hunks"]):
                # Removed text needs its stable surrounding anchor to reverse;
                # the reconstructed whole-file base hash must still match.
                if not hunk["after"]:
                    anchor = b"// A buffered file.\n"
                    if data.count(anchor) != 1:
                        parser.error("ambiguous removed-hunk reconstruction")
                    data = data.replace(anchor, hunk["before"].encode() + anchor)
                else:
                    data = data.replace(hunk["after"].encode(), hunk["before"].encode())
        if digest(data) != item["base_sha256"]:
            parser.error("unknown base/result source hash: " + item["path"])
        BASE[item["path"]] = data
    unittest.main(argv=[__file__] + rest)

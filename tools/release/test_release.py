import json
import os
import shutil
import sys
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from version import derive, generate, metadata
from artifacts import archive, digest, seal, verify, verify_unit_report
from draft import draft

ROOT = Path(__file__).resolve().parents[2]
# Stable test vector: changing the real product version must not require
# rewriting test-fixture identities or editing a second version authority.
BASE = {"schemaVersion": 1, "major": 1, "minor": 1, "patch": 0,
        "prerelease": "beta", "sequence": 1, "abi": 1}
REVISION = "a" * 40


class Versions(unittest.TestCase):
    def test_checked_in_manifest_is_valid(self):
        derive(json.loads((ROOT / "version.json").read_text()))

    def test_known_identity(self):
        value = derive(BASE)
        self.assertEqual((value["semver"], value["msi"], value["odbc"]),
                         ("1.1.0-beta.1", "1.1.1", "01.01.0000"))
        self.assertGreater(tuple(map(int, value["msi"].split("."))), (1, 0, 65535))

    def test_order_across_all_stages_patch_minor_major(self):
        variants = [(1, 1, 0, "beta", 1), (1, 1, 0, "beta", 499),
                    (1, 1, 0, "rc", 1), (1, 1, 0, "rc", 499), (1, 1, 0, "", 0),
                    (1, 1, 1, "beta", 1), (1, 1, 64, "", 0), (1, 2, 0, "beta", 1),
                    (2, 0, 0, "beta", 1), (99, 99, 64, "", 0)]
        numbers = []
        for major, minor, patch, stage, seq in variants:
            value = derive({**BASE, "major": major, "minor": minor, "patch": patch,
                            "prerelease": stage, "sequence": seq})
            triple = tuple(map(int, value["msi"].split(".")))
            self.assertTrue(all(x <= bound for x, bound in zip(triple, (255, 255, 65535))))
            numbers.append(triple)
        self.assertEqual(sorted(set(numbers)), numbers)

    def test_invalid_manifest_rejected(self):
        for key, value in [("major", 256), ("major", 100), ("minor", 256), ("patch", 65),
                           ("sequence", 500), ("sequence", 0), ("major", True), ("major", "1"),
                           ("abi", 0), ("prerelease", "beta;run"), ("schemaVersion", 2)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                derive({**BASE, key: value})
        with self.assertRaises(ValueError):
            derive({**BASE, "prerelease": ""})

    def make_repository(self, source):
        source.mkdir(parents=True, exist_ok=True)
        (source / "version.json").write_text(json.dumps(BASE))
        def git(*args):
            return subprocess.check_output(["git", "-C", str(source), *args],
                                           text=True, stderr=subprocess.DEVNULL).strip()
        git("init")
        git("add", "version.json")
        git("-c", "user.name=Release Control Test", "-c", "user.email=test@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "-m", "fixture")
        return git("rev-parse", "HEAD")

    def test_export_build_metadata_without_git_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "export"
            source.mkdir()
            (source / "version.json").write_text(json.dumps(BASE))
            output = Path(tmp) / "generated"
            environment = {**os.environ, "PATH": ""}
            subprocess.run([sys.executable, str(ROOT / "tools/release/version.py"),
                            "--source", str(source), "--output", str(output)],
                           env=environment, check=True)
            value = json.loads((output / "odbcpp-version.json").read_text())
            self.assertEqual(value["semver"], "1.1.0-beta.1")
            self.assertEqual(value["display"], "1.1.0-beta.1+export.unknown")
            self.assertEqual(value["sourceProvenance"], "export")
            self.assertIsNone(value["sourceRevision"])
            self.assertIsNone(value["trackedSourceClean"])
            # Consume generated identity through actual CMake; no repository exists.
            script = Path(tmp) / "export.cmake"
            script.write_text(f'include("{output.as_posix()}/odbcpp-version.cmake")\n'
                              'if(NOT ODBCPP_VERSION_CORE STREQUAL "1.1.0")\n'
                              '  message(FATAL_ERROR "Export version changed")\nendif()\n')
            subprocess.run([shutil.which("cmake"), "-P", str(script)], env=environment, check=True)

    def test_nested_export_cannot_borrow_parent_repository_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp) / "unrelated"
            parent_head = self.make_repository(parent)
            child = parent / "vendor" / "odbcpp"
            child.mkdir(parents=True)
            (child / "version.json").write_text(json.dumps(BASE))
            value = metadata(child)
            self.assertEqual(value["sourceProvenance"], "export")
            self.assertEqual(value["provenanceReason"], "no-repository-metadata")
            self.assertIsNone(value["sourceRevision"])
            self.assertNotIn(parent_head[:12], value["display"])

    def test_exact_git_repository_and_git_unavailable_are_distinct(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "project"
            head = self.make_repository(source)
            value = metadata(source)
            self.assertEqual(value["sourceProvenance"], "git")
            self.assertEqual(value["sourceRevision"], head)
            self.assertIs(value["trackedSourceClean"], True)
            with mock.patch("version.subprocess.check_output", side_effect=FileNotFoundError):
                unknown = metadata(source)
            self.assertEqual(unknown["provenanceReason"], "git-unavailable")
            self.assertIsNone(unknown["sourceRevision"])
            self.assertIsNone(unknown["trackedSourceClean"])

    def test_cmake_generated_identity_and_prerelease_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            cmake_dir = directory.as_posix()
            cmake_root = ROOT.as_posix()
            value = {**derive(BASE), "sourceProvenance": "git", "sourceRevision": REVISION, "trackedSourceClean": True, "display": "1.1.0-beta.1+gaaaaaaaaaaaa"}
            generate(value, directory)
            for ack, expected in [("", False), ("1.1.0-beta.2", False), ("1.1.0-beta.1", True)]:
                script = directory / "check.cmake"
                script.write_text(f'''cmake_minimum_required(VERSION 3.20)
include("{cmake_dir}/odbcpp-version.cmake")
include("{cmake_root}/cmake/VersionCompatibility.cmake")
odbcpp_write_package_version("{cmake_dir}/ConfigVersion.cmake")
set(ODBCPP_ACCEPT_PRERELEASE "{ack}")
set(PACKAGE_FIND_VERSION "1.1.0")
set(PACKAGE_FIND_VERSION_MAJOR 1)
set(PACKAGE_FIND_VERSION_MINOR 1)
set(PACKAGE_FIND_VERSION_PATCH 0)
include("{cmake_dir}/ConfigVersion.cmake")
if(NOT "${{PACKAGE_VERSION_COMPATIBLE}}" STREQUAL "{'TRUE' if expected else 'FALSE'}")
  message(FATAL_ERROR "Unexpected compatibility")
endif()
''')
                subprocess.run(["cmake", "-P", str(script)], check=True)
            self.assertIn("FILEVERSION 1,1,1,0", (directory / "odbcpp-version.rc").read_text())


class Artifacts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.package = self.root / "package"
        self.out = self.root / "out"
        self.value = {**derive(BASE), "sourceProvenance": "git", "sourceRevision": REVISION, "trackedSourceClean": True, "display": "test"}
        generate(self.value, self.build / "version")
        self.stage = self.package / "payload-1.1.1"
        self.stage.mkdir(parents=True)
        (self.stage / "odbcpp.dll").write_bytes(b"synthetic fixture; not a real DLL")
        (self.stage / "LICENSE").write_text("fixture license")
        for name in ("spdlog-LICENSE.txt", "fmt-LICENSE.txt"):
            (self.stage / name).write_text("fixture notice")
        for name in ("odbcpp-crypto-manifest.json", "odbcpp-crypto-artifact-evidence.json"):
            (self.stage / name).write_text('{"fixture": true, "requestedLinkage": "SYSTEM_SHARED"}')
        for name in ("package-load.log", "release-msi-inspection.log"):
            (self.build / name).write_text("Synthetic control fixture only; not package execution")
        (self.stage / "odbcpp-version.json").write_text(json.dumps(self.value))
        self.inventory = {"sourceRevision": REVISION, "packagingRevision": REVISION, "targetDatabase": "POSTGRESQL",
                          "lifecycleFixture": False, "semanticVersion": self.value["semver"], "packageVersion": self.value["msi"],
                          "files": [{"file": "odbcpp.dll", "sha256": digest(self.stage / "odbcpp.dll")} ]}
        (self.stage / "inventory.json").write_text(json.dumps(self.inventory))
        (self.package / "odbcpp-postgresql-1.1.0-beta.1-x64.msi").write_bytes(b"synthetic MSI fixture")
        (self.build / "release-unit.xml").write_text('<testsuite tests="3" failures="0" errors="0" skipped="0"/>')

    def make(self, product="POSTGRESQL"):
        seal(self.build, self.package, self.out, REVISION, product)

    def test_seal_verify_and_detect_tampering(self):
        self.make()
        self.assertEqual(verify(self.out, REVISION)["qualification"]["unitCases"], 3)
        with self.assertRaises(ValueError):
            verify(self.out, "b" * 40)
        (self.out / "LICENSE").write_text("modified")
        with self.assertRaises(ValueError):
            verify(self.out, REVISION)

    def test_reject_fixture_dirty_stale_and_payload_mutation(self):
        self.inventory["lifecycleFixture"] = True
        (self.stage / "inventory.json").write_text(json.dumps(self.inventory))
        with self.assertRaises(ValueError):
            self.make()
        self.inventory["lifecycleFixture"] = False
        (self.stage / "inventory.json").write_text(json.dumps(self.inventory))
        (self.stage / "odbcpp.dll").write_text("changed after packaging")
        with self.assertRaises(ValueError):
            self.make()
        for key, val in [("trackedSourceClean", False), ("sourceRevision", "b" * 40), ("sourceProvenance", "export")]:
            generate({**self.value, key: val}, self.build / "version")
            with self.assertRaises(ValueError):
                self.make()

    def test_no_empty_failed_or_skipped_unit_receipts(self):
        for xml in ['<testsuite tests="0"/>', '<testsuite tests="1" skipped="1"/>', '<testsuite tests="1" failures="1"/>', '<testsuite tests="1" errors="1"/>']:
            path = self.build / "release-unit.xml"
            path.write_text(xml)
            with self.assertRaises(ValueError):
                verify_unit_report(path)

    def test_checksum_inventory_rejects_extra_file_and_path(self):
        self.make()
        extra = self.out / "surprise.exe"
        extra.write_text("x")
        with self.assertRaises(ValueError):
            verify(self.out, REVISION)
        extra.unlink()
        with (self.out / "SHA256SUMS").open("a") as stream:
            stream.write("0" * 64 + "  ../escape\n")
        with self.assertRaises(ValueError):
            verify(self.out, REVISION)

    def test_archive_reuses_bytes_and_refuses_escape(self):
        link = self.stage / "libdriver.so"
        link.symlink_to("odbcpp.dll")
        target = self.root / "profile.tar.gz"
        archive(self.stage, target, "linux-ubuntu24.04-amd64-unixodbc-ucs2")
        with tarfile.open(target) as tar:
            body = tar.extractfile("linux-ubuntu24.04-amd64-unixodbc-ucs2/libdriver.so").read()
            self.assertEqual(body, (self.stage / "odbcpp.dll").read_bytes())
        link.unlink()
        link.symlink_to(self.build / "release-unit.xml")
        with self.assertRaises(ValueError):
            archive(self.stage, self.root / "bad.tar.gz", "linux-amd64-unixodbc-ucs2")

    def test_draft_only_reuses_assets_and_refuses_existing_release(self):
        # Duplicate only synthetic fixtures to exercise publication control.
        release = self.root / "release"
        self.out = release / "POSTGRESQL"
        self.make()
        self.inventory["targetDatabase"] = "REDSHIFT"
        (self.stage / "inventory.json").write_text(json.dumps(self.inventory))
        (self.package / "odbcpp-redshift-1.1.0-beta.1-x64.msi").write_bytes(b"synthetic RS MSI")
        self.out = release / "REDSHIFT"
        self.make("REDSHIFT")
        with mock.patch("draft.subprocess.run") as run:
            run.return_value.returncode = 0
            with self.assertRaises(ValueError):
                draft(release, REVISION)
            self.assertEqual(run.call_count, 1)
        missing = subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"HTTP 404")
        existing_draft = subprocess.CompletedProcess([], 0, stdout=json.dumps([
            [{"tag_name": "v0.0.1", "draft": False}],
            [{"tag_name": "v1.1.0-beta.1", "draft": True}]]).encode(), stderr=b"")
        with mock.patch("draft.subprocess.run", side_effect=[missing, missing, existing_draft]) as run:
            with self.assertRaisesRegex(ValueError, "draft already exists"):
                draft(release, REVISION)
            self.assertEqual(run.call_count, 3)
            self.assertIn("--paginate", run.call_args.args[0])
            self.assertFalse(any(call.args[0][1:3] == ["release", "create"] for call in run.call_args_list))
        refused_listings = [
            (subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"HTTP 403"), RuntimeError),
            (subprocess.CompletedProcess([], 0, stdout=b'{"unexpected": true}', stderr=b""), ValueError)]
        for listing, exception in refused_listings:
            with mock.patch("draft.subprocess.run", side_effect=[missing, missing, listing]) as run:
                with self.assertRaises(exception):
                    draft(release, REVISION)
                self.assertEqual(run.call_count, 3)
        def absent_then_create(command, **kwargs):
            if "--paginate" in command:
                return subprocess.CompletedProcess(command, 0, stdout=b"[[]]", stderr=b"")
            if command[1] == "api":
                return missing
            return subprocess.CompletedProcess(command, 0)
        with mock.patch("draft.subprocess.run", side_effect=absent_then_create) as run:
            draft(release, REVISION)
            command = run.call_args.args[0]
            self.assertIn("--draft", command)
            self.assertIn("--prerelease", command)
            self.assertIn(REVISION, command)
            self.assertNotIn("--clobber", command)


if __name__ == "__main__":
    unittest.main()

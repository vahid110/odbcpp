import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from version import derive, generate
from artifacts import archive, digest, seal, verify, verify_unit_report
from draft import draft

ROOT = Path(__file__).resolve().parents[2]
BASE = json.loads((ROOT / "version.json").read_text())
REVISION = "a" * 40


class Versions(unittest.TestCase):
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

    def test_cmake_generated_identity_and_prerelease_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            cmake_dir = directory.as_posix()
            cmake_root = ROOT.as_posix()
            value = {**derive(BASE), "sourceRevision": REVISION, "trackedSourceClean": True, "display": "1.1.0-beta.1+gaaaaaaaaaaaa"}
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
        self.value = {**derive(BASE), "sourceRevision": REVISION, "trackedSourceClean": True, "display": "test"}
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
        for key, val in [("trackedSourceClean", False), ("sourceRevision", "b" * 40)]:
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
        with mock.patch("draft.subprocess.run") as run:
            run.return_value.returncode = 1
            run.return_value.stderr = b"HTTP 404"
            draft(release, REVISION)
            command = run.call_args.args[0]
            self.assertIn("--draft", command)
            self.assertIn("--prerelease", command)
            self.assertIn(REVISION, command)
            self.assertNotIn("--clobber", command)


if __name__ == "__main__":
    unittest.main()

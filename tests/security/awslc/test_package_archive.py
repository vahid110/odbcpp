"""Positive and adversarial checks for the internal package evidence fixture."""
import io
import hashlib
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

from record_provider_identity import validate as validate_identity
from test_relocated_package import extract_verified, inventory, validate_manifest, validate_loaded_providers


class PackageArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "staged"
        (self.package / "lib").mkdir(parents=True)
        self.library = self.package / "lib/provider.so.1"
        self.library.write_bytes(b"provider bytes")
        self.library.chmod(0o755)
        (self.package / "lib/provider.so").symlink_to("provider.so.1")
        self.expected = inventory(self.package)
        self.archive = self.root / "package.tar.gz"

    def pack(self, extra=None):
        with tarfile.open(self.archive, "w:gz") as output:
            output.add(self.package, arcname="odbcpp-awslc-proof")
            if extra:
                output.addfile(extra, io.BytesIO(b""))

    def extract(self):
        return extract_verified(self.archive, self.root / "extracted", self.expected)

    def test_roundtrip_preserves_bytes_symlink_and_mode(self):
        self.pack()
        result = self.extract()
        self.assertEqual(inventory(result), self.expected)
        self.assertTrue((result / "lib/provider.so").is_symlink())

    def test_changed_bytes_rejected(self):
        self.library.write_bytes(b"substituted provider")
        self.pack()
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.extract()

    def test_changed_mode_rejected(self):
        self.library.chmod(0o644)
        self.pack()
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.extract()

    def test_changed_symlink_rejected(self):
        (self.package / "lib/provider.so.2").write_bytes(self.library.read_bytes())
        self.expected = inventory(self.package)
        link = self.package / "lib/provider.so"
        link.unlink()
        link.symlink_to("provider.so.2")
        self.pack()
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.extract()

    def test_missing_member_rejected(self):
        (self.package / "lib/provider.so").unlink()
        self.pack()
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.extract()

    def test_unexpected_member_rejected(self):
        (self.package / "extra").write_bytes(b"extra")
        self.pack()
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.extract()

    def test_duplicate_member_rejected(self):
        self.pack(tarfile.TarInfo("odbcpp-awslc-proof/lib/provider.so.1"))
        with self.assertRaisesRegex(RuntimeError, "Duplicate"):
            self.extract()

    def test_traversal_rejected(self):
        self.pack(tarfile.TarInfo("../escaped"))
        with self.assertRaises(tarfile.FilterError):
            self.extract()
        self.assertFalse((self.root / "escaped").exists())

    def test_external_symlink_rejected_before_archiving(self):
        (self.package / "escape").symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "escaped"):
            inventory(self.package)

    def test_archive_external_symlink_rejected(self):
        link = tarfile.TarInfo("odbcpp-awslc-proof/lib/escape")
        link.type = tarfile.SYMTYPE
        link.linkname = "../../../outside"
        self.pack(link)
        with self.assertRaises(tarfile.FilterError):
            self.extract()


class PackageManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        licenses = {}
        for name in ("aws-lc/LICENSE", "aws-lc/NOTICE", "spdlog/LICENSE", "fmt/LICENSE"):
            path = self.root / "licenses" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("license: " + name).encode())
            licenses["licenses/" + name] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.manifest = {
            "schemaVersion": 1, "provider": "AWS_LC", "linkage": "BUNDLED_SHARED",
            "fipsRequested": False, "qualificationClaimed": False,
            "sourceProvenanceVerified": False, "licenseFiles": licenses,
            "declaredSourceRecipes": {
                "aws-lc": {"commit": "a" * 40, "archiveSha256": "b" * 64,
                           "sourceOverride": False},
                "spdlog": {"version": "1.17.0", "archiveSha256": "c" * 64,
                           "sourceOverride": True},
                "fmt": {"bundledBy": "spdlog", "versionNumber": 120100},
            },
        }
        self.save()

    def save(self):
        (self.root / "package-manifest.json").write_text(json.dumps(self.manifest))

    def test_shared_and_static_profiles(self):
        validate_manifest(self.root, True)
        self.manifest["linkage"] = "BUNDLED_STATIC"
        self.save()
        validate_manifest(self.root, False)

    def test_wrong_profile_and_premature_claims(self):
        for field, value in (("provider", "OPENSSL"), ("linkage", "BUNDLED_STATIC"),
                             ("qualificationClaimed", True), ("fipsRequested", True),
                             ("sourceProvenanceVerified", True)):
            with self.subTest(field=field):
                original = self.manifest[field]
                self.manifest[field] = value
                self.save()
                with self.assertRaisesRegex(RuntimeError, "profile"):
                    validate_manifest(self.root, True)
                self.manifest[field] = original

    def test_missing_and_malformed_recipes(self):
        for name, field, bad in (("aws-lc", "commit", "bad"),
                                 ("spdlog", "archiveSha256", "bad"),
                                 ("spdlog", "sourceOverride", "false"),
                                 ("fmt", "versionNumber", True)):
            with self.subTest(field=field):
                recipe = self.manifest["declaredSourceRecipes"][name]
                original = recipe[field]
                recipe[field] = bad
                self.save()
                with self.assertRaisesRegex(RuntimeError, "recipe"):
                    validate_manifest(self.root, True)
                recipe[field] = original
        del self.manifest["declaredSourceRecipes"]
        self.save()
        with self.assertRaisesRegex(RuntimeError, "recipe"):
            validate_manifest(self.root, True)

    def test_changed_license_rejected(self):
        (self.root / "licenses/aws-lc/LICENSE").write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "hash mismatch"):
            validate_manifest(self.root, True)

    def test_missing_license_rejected(self):
        (self.root / "licenses/aws-lc/NOTICE").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing"):
            validate_manifest(self.root, True)

    def test_incomplete_inventory_rejected(self):
        del self.manifest["licenseFiles"]["licenses/fmt/LICENSE"]
        self.save()
        with self.assertRaisesRegex(RuntimeError, "inventory"):
            validate_manifest(self.root, True)


class ProviderOriginTests(unittest.TestCase):
    def test_selected_paths_and_aliases_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            provider = root / "provider.so.1"
            provider.write_bytes(b"provider")
            alias = root / "provider.so"
            alias.symlink_to(provider.name)
            validate_loaded_providers([str(provider)], [str(alias)])
            validate_loaded_providers([], [])

    def test_identical_external_copy_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            selected = root / "selected.so"
            external = root / "external.so"
            selected.write_bytes(b"same bytes")
            external.write_bytes(selected.read_bytes())
            with self.assertRaisesRegex(RuntimeError, "origin mismatch"):
                validate_loaded_providers([str(external)], [str(selected)])
            with self.assertRaisesRegex(RuntimeError, "origin mismatch"):
                validate_loaded_providers([str(selected), str(external)], [str(selected)])
            with self.assertRaisesRegex(RuntimeError, "origin mismatch"):
                validate_loaded_providers([], [str(selected)])


class IdentityEvidenceTests(unittest.TestCase):
    def test_identity_and_policy_match(self):
        lines = ["AWS_LC", "OpenSSL 1.1.1 (compatible; AWS-LC 5.10.0)",
                 "AWS-LC 5.10.0", "0", "771", "1", "1"]
        evidence = validate_identity(lines, {"compileVersion": "AWS-LC 5.10.0"})
        self.assertFalse(evidence["activeFips"])
        for index, bad in ((0, "OPENSSL"), (1, "wrong compile version"),
                           (1, "OpenSSL 1.1.1 (compatible; AWS-LC 5.10.01)"),
                           (2, "AWS-LC 5.9.0"), (3, "1"), (4, "770"),
                           (5, "0"), (6, "0")):
            with self.subTest(index=index):
                altered = list(lines)
                altered[index] = bad
                with self.assertRaisesRegex(RuntimeError, "mismatch"):
                    validate_identity(altered, {"compileVersion": "AWS-LC 5.10.0"})
        with self.assertRaisesRegex(RuntimeError, "Incomplete"):
            validate_identity(lines[:-1], {"compileVersion": "AWS-LC 5.10.0"})


if __name__ == "__main__":
    unittest.main()

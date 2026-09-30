"""Positive and adversarial checks for the internal package evidence fixture."""
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from test_relocated_package import extract_verified, inventory


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


if __name__ == "__main__":
    unittest.main()

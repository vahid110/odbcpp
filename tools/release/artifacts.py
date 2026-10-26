#!/usr/bin/env python3
"""Seal and verify exact release candidates; never rebuild, install or publish."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import xml.etree.ElementTree as ET
from version import derive


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hashlib.sha256(stream.read()).hexdigest()


def regular(path):
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"Expected regular file: {path}")
    return path


def verify_unit_report(path):
    root = ET.parse(regular(path)).getroot()
    suites = [root] if root.tag == "testsuite" else list(root.findall("testsuite"))
    if not suites or sum(int(s.get("tests", "0")) for s in suites) == 0:
        raise ValueError("Unit report has no executed cases")
    if any(int(s.get(key, "0")) for s in suites for key in ("failures", "errors", "skipped", "disabled")):
        raise ValueError("Unit report contains failures/errors/skips")
    return sum(int(s.get("tests", "0")) for s in suites)


def seal(build, package, output, expected_revision, product):
    version = json.loads(regular(build / "version/odbcpp-version.json").read_text(encoding="utf-8-sig"))
    if (version["sourceRevision"] != expected_revision or not re.fullmatch(r"[0-9a-f]{40}", expected_revision)
            or version["trackedSourceClean"] is not True or not version["prerelease"]):
        raise ValueError("Require clean exact-source prerelease metadata")
    derived = derive({key: version[key] for key in ("schemaVersion", "major", "minor", "patch", "prerelease", "sequence", "abi")})
    if any(version[key] != val for key, val in derived.items()):
        raise ValueError("Generated version representations disagree")
    inventory = json.loads(regular(package / f'payload-{version["msi"]}/inventory.json').read_text(encoding="utf-8-sig"))
    if (inventory["sourceRevision"] != expected_revision or inventory["packagingRevision"] != expected_revision
            or inventory["targetDatabase"] != product or inventory["lifecycleFixture"] is not False
            or inventory["semanticVersion"] != version["semver"] or inventory["packageVersion"] != version["msi"]):
        raise ValueError("Package source, identity or release status differs")
    stage = package / f'payload-{version["msi"]}'
    for item in inventory["files"]:
        name = item["file"]
        if Path(name).name != name or digest(regular(stage / name)).lower() != item["sha256"].lower():
            raise ValueError("Staged runtime does not match inventory")
    unit_count = verify_unit_report(build / "release-unit.xml")
    output.mkdir(parents=True, exist_ok=False)
    msi = regular(package / f'odbcpp-{product.lower()}-{version["semver"]}-x64.msi')
    files = [msi, regular(stage / "inventory.json"), regular(stage / "odbcpp-version.json"),
             regular(stage / "LICENSE"), regular(build / "release-unit.xml"),
             regular(build / "package-load.log"), regular(build / "release-msi-inspection.log"),
             regular(stage / "odbcpp-crypto-manifest.json"), regular(stage / "odbcpp-crypto-artifact-evidence.json")]
    for path in files:
        shutil.copyfile(path, output / path.name)
    payload = {p.name: digest(p) for p in sorted(output.iterdir())}
    receipt = {"schemaVersion": 1, "sourceRevision": expected_revision, "semanticVersion": version["semver"],
               "product": product, "profile": "windows-2025-x64-windowsdm-ucs2-openssl-bundled-shared",
               "signed": False, "qualification": {"unitCases": unit_count, "installerLifecycle": "pending",
                   "live": "pending", "releaseAccepted": False}, "files": payload}
    (output / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    payload["provenance.json"] = digest(output / "provenance.json")
    (output / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in sorted(payload.items())))


def verify(directory, revision):
    receipt = json.loads(regular(directory / "provenance.json").read_text())
    if receipt["sourceRevision"] != revision:
        raise ValueError("Artifact source differs from selected immutable revision")
    listed = {}
    for line in regular(directory / "SHA256SUMS").read_text().splitlines():
        sha, name = line.split("  ", 1)
        if not re.fullmatch(r"[0-9a-f]{64}", sha) or Path(name).name != name or name in listed:
            raise ValueError("Invalid checksum entry")
        listed[name] = sha
        if digest(regular(directory / name)) != sha:
            raise ValueError(f"Artifact changed: {name}")
    if listed.pop("provenance.json", None) is None or listed != receipt["files"]:
        raise ValueError("Checksum and provenance inventories differ")
    if set(p.name for p in directory.iterdir()) != set(receipt["files"]) | {"provenance.json", "SHA256SUMS"}:
        raise ValueError("Unexpected artifact files")
    return receipt


def archive(stage, output, profile):
    """Portable staging foundation, not a declaration of OS/ABI compatibility."""
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]+", profile):
        raise ValueError("Use an explicit safe runtime/DM/architecture profile label")
    files = sorted(stage.rglob("*"))
    names = {p.name for p in files}
    if not {"LICENSE", "spdlog-LICENSE.txt", "fmt-LICENSE.txt", "odbcpp-version.json", "odbcpp-crypto-manifest.json"} <= names:
        raise ValueError("Stage must contain project/dependency notices and generated identities")
    manifests = [p for p in files if p.name == "odbcpp-crypto-manifest.json"]
    if len(manifests) != 1:
        raise ValueError("Expected one crypto profile manifest")
    crypto = json.loads(manifests[0].read_text())
    if crypto.get("requestedLinkage") not in ("SYSTEM_SHARED", "BUNDLED_SHARED", "BUNDLED_STATIC"):
        raise ValueError("Unknown archive crypto linkage")
    if crypto["requestedLinkage"] != "SYSTEM_SHARED" and "OpenSSL-LICENSE.txt" not in names:
        raise ValueError("Bundled crypto requires its OpenSSL license notice")
    root = stage.resolve()
    for path in files:
        if not (path.is_file() or path.is_dir()):
            raise ValueError("Archive staging contains a special or dangling file")
        if path.is_symlink() and (not path.resolve().is_relative_to(root) or path.is_dir()):
            raise ValueError("Archive symlinks must resolve to files inside staging")
    # cmake --install --component ODBCPPDriverRuntime creates staging; this never builds.
    with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz", dereference=True) as tar:
        for path in files:
            tar.add(path, arcname=profile + "/" + path.relative_to(stage).as_posix(), recursive=False)
    output.with_suffix(output.suffix + ".sha256").write_text(f"{digest(output)}  {output.name}\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("seal")
    for name in ("build", "package", "output"):
        make.add_argument("--" + name, type=Path, required=True)
    make.add_argument("--revision", required=True)
    make.add_argument("--product", choices=("POSTGRESQL", "REDSHIFT"), required=True)
    check = sub.add_parser("verify")
    check.add_argument("--directory", type=Path, required=True)
    check.add_argument("--revision", required=True)
    pack = sub.add_parser("archive")
    pack.add_argument("--stage", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    pack.add_argument("--profile", required=True)
    args = parser.parse_args()
    if args.command == "seal":
        seal(args.build, args.package, args.output, args.revision, args.product)
    elif args.command == "verify":
        print(json.dumps(verify(args.directory, args.revision), indent=2))
    else:
        archive(args.stage, args.output, args.profile)

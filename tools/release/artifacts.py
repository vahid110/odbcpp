#!/usr/bin/env python3
"""Seal and verify exact release candidates; never rebuild, install or publish."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tarfile
from version import derive
from elf_dependencies import profile_contract, scan
from paths import member, regular, selected_root, selected_output, fresh_directory

# Load only the existing bounded parser from this source tree. Never fall back
# to cwd/import-path modules when release evidence parsing is unavailable.
_xml_spec = importlib.util.spec_from_file_location(
    "odbcpp_xml_evidence", Path(__file__).resolve().parents[1] / "odbcpp_xml_evidence.py")
_xml_module = importlib.util.module_from_spec(_xml_spec)
_xml_spec.loader.exec_module(_xml_module)
parse_xml_file = _xml_module.parse_xml_file


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hashlib.sha256(stream.read()).hexdigest()


def verify_unit_report(path):
    root = parse_xml_file(regular(path))
    suites = [root] if root.tag == "testsuite" else list(root.findall("testsuite"))
    if not suites or sum(int(s.get("tests", "0")) for s in suites) == 0:
        raise ValueError("Unit report has no executed cases")
    if any(int(s.get(key, "0")) for s in suites for key in ("failures", "errors", "skipped", "disabled")):
        raise ValueError("Unit report contains failures/errors/skips")
    return sum(int(s.get("tests", "0")) for s in suites)


def seal(build, package, output, expected_revision, product):
    build, package = selected_root(build), selected_root(package)
    if product not in ("POSTGRESQL", "REDSHIFT"):
        raise ValueError("Unknown package product")
    version = json.loads(regular(member(build, "version/odbcpp-version.json")).read_text(encoding="utf-8-sig"))
    if (version.get("sourceProvenance") != "git" or version["sourceRevision"] != expected_revision or not re.fullmatch(r"[0-9a-f]{40}", expected_revision)
            or version["trackedSourceClean"] is not True or not version["prerelease"]):
        raise ValueError("Require clean exact-source prerelease metadata")
    derived = derive({key: version[key] for key in ("schemaVersion", "major", "minor", "patch", "prerelease", "sequence", "abi")})
    if any(version[key] != val for key, val in derived.items()):
        raise ValueError("Generated version representations disagree")
    inventory = json.loads(regular(member(package, f'payload-{version["msi"]}/inventory.json')).read_text(encoding="utf-8-sig"))
    if (inventory["sourceRevision"] != expected_revision or inventory["packagingRevision"] != expected_revision
            or inventory["targetDatabase"] != product or inventory["lifecycleFixture"] is not False
            or inventory["semanticVersion"] != version["semver"] or inventory["packageVersion"] != version["msi"]):
        raise ValueError("Package source, identity or release status differs")
    stage = member(package, f'payload-{version["msi"]}')
    for item in inventory["files"]:
        name = item["file"]
        if Path(name).name != name or digest(regular(member(stage, name))).lower() != item["sha256"].lower():
            raise ValueError("Staged runtime does not match inventory")
    support = profile_contract("windows-x86-64-windowsdm-ucs2-openssl-bundled-shared")
    crypto = json.loads(regular(member(stage, "odbcpp-crypto-manifest.json")).read_text())
    if crypto.get("provider") != support["crypto"]["provider"] or crypto.get("requestedLinkage") != support["crypto"]["linkage"]:
        raise ValueError("Staged crypto identity differs from Windows release profile")
    unit_count = verify_unit_report(member(build, "release-unit.xml"))
    msi = regular(member(package, f'odbcpp-{product.lower()}-{version["semver"]}-x64.msi'))
    files = [msi, regular(member(stage, "inventory.json")), regular(member(stage, "odbcpp-version.json")),
             regular(member(stage, "LICENSE")), regular(member(build, "release-unit.xml")),
             regular(member(build, "package-load.log")), regular(member(build, "release-msi-inspection.log")),
             regular(member(stage, "odbcpp-crypto-manifest.json")), regular(member(stage, "odbcpp-crypto-artifact-evidence.json"))]
    output = fresh_directory(output)
    for path in files:
        shutil.copyfile(path, member(output, path.name))
    payload = {p.name: digest(p) for p in sorted(output.iterdir())}
    receipt = {"schemaVersion": 1, "sourceRevision": expected_revision, "semanticVersion": version["semver"],
               "product": product, "profile": "windows-x86-64-windowsdm-ucs2-openssl-bundled-shared",
               "buildEnvironment": "windows-2025",
               "runtimeSupport": support,
               "signed": False, "qualification": {"unitCases": unit_count, "installerLifecycle": "pending",
                   "live": "pending", "releaseAccepted": False}, "files": payload}
    (output / "provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    payload["provenance.json"] = digest(output / "provenance.json")
    (output / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in sorted(payload.items())))


def verify(directory, revision):
    directory = selected_root(directory)
    receipt = json.loads(regular(directory / "provenance.json").read_text())
    if receipt["sourceRevision"] != revision:
        raise ValueError("Artifact source differs from selected immutable revision")
    listed = {}
    for line in regular(directory / "SHA256SUMS").read_text().splitlines():
        sha, name = line.split("  ", 1)
        if not re.fullmatch(r"[0-9a-f]{64}", sha) or Path(name).name != name or name in listed:
            raise ValueError("Invalid checksum entry")
        listed[name] = sha
        if digest(regular(member(directory, name))) != sha:
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
    stage = selected_root(stage)
    output = selected_output(output)
    checksum = selected_output(output.with_suffix(output.suffix + ".sha256"))
    dependency_report = selected_output(output.with_suffix(output.suffix + ".dependencies.json"))
    # Preserve existing artifacts/evidence even when qualification will fail.
    for destination in (output, checksum, dependency_report):
        if destination.exists() or destination.is_symlink():
            raise FileExistsError(f"Release destination already occupied: {destination}")
    files = sorted(stage.rglob("*"))
    root = stage.resolve()
    for path in files:
        member(stage, path.relative_to(stage).as_posix())
        if not (path.is_file() or path.is_dir()):
            raise ValueError("Archive staging contains a special or dangling file")
        if path.is_symlink() and (not path.resolve().is_relative_to(root) or path.is_dir()):
            raise ValueError("Archive symlinks must resolve to files inside staging")
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
    # Reject an ELF disguised under a non-Linux profile. Linux archives require
    # measured baseline ceilings; pending support policy cannot silently pass.
    has_elf = False
    for path in files:
        if path.is_file():
            with path.open("rb") as candidate:
                has_elf = has_elf or candidate.read(4) == b"\x7fELF"
    if profile.startswith("linux-") or has_elf:
        contract = profile_contract(profile)
        if crypto["requestedLinkage"] != contract["crypto"]["linkage"] or crypto.get("provider") != contract["crypto"]["provider"]:
            raise ValueError("Stage crypto linkage differs from Linux profile")
        report = scan(stage, profile)
        with dependency_report.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(report, indent=2) + "\n")
        if report["status"] != "pass":
            raise ValueError("Linux dependency check " + report["status"] + "; see dependency report")
    # cmake --install --component ODBCPPDriverRuntime creates staging; this never builds.
    with output.open("xb") as stream, tarfile.open(fileobj=stream, mode="w:gz", dereference=True) as tar:
        for path in files:
            tar.add(path, arcname=profile + "/" + path.relative_to(stage).as_posix(), recursive=False)
    with checksum.open("x", encoding="utf-8") as stream:
        stream.write(f"{digest(output)}  {output.name}\n")


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

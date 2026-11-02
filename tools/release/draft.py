#!/usr/bin/env python3
"""Create a draft from verified candidate bytes; publishing is deliberately absent."""
import argparse
import json
from pathlib import Path
import subprocess
import re
import tempfile
import tarfile
from artifacts import digest
from artifacts import verify


def draft(directory, revision):
    receipts = [verify(directory / product, revision) for product in ("POSTGRESQL", "REDSHIFT")]
    if receipts[0]["semanticVersion"] != receipts[1]["semanticVersion"]:
        raise ValueError("Product versions differ")
    for product, receipt in zip(("POSTGRESQL", "REDSHIFT"), receipts):
        if receipt["product"] != product:
            raise ValueError("Artifact product differs from its directory")
    version = receipts[0]["semanticVersion"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+-(beta|rc)\.[1-9][0-9]*", version):
        raise ValueError("Only prerelease drafts are supported")
    tag = "v" + version
    # Never clobber existing assets or relabel an already-published version.
    for endpoint in (f"releases/tags/{tag}", f"git/ref/tags/{tag}"):
        result = subprocess.run(["gh", "api", "repos/{owner}/{repo}/" + endpoint], capture_output=True)
        if result.returncode == 0:
            raise ValueError("Release or tag already exists; never replace an existing version")
        if b"HTTP 404" not in result.stderr:
            raise RuntimeError("Cannot verify absence of existing release/tag; refusing draft creation")
    # The by-tag endpoint can return 404 for an unpublished draft without a tag.
    # The authenticated releases listing includes drafts; inspect every page.
    listing = subprocess.run(["gh", "api", "--paginate", "--slurp",
                              "repos/{owner}/{repo}/releases?per_page=100"], capture_output=True)
    if listing.returncode != 0:
        raise RuntimeError("Cannot verify absence of existing drafts; refusing draft creation")
    pages = json.loads(listing.stdout)
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValueError("Unexpected paginated releases response")
    for page in pages:
        for release in page:
            if not isinstance(release, dict) or not isinstance(release.get("tag_name"), str):
                raise ValueError("Unexpected release identity in listing")
            if release["tag_name"] == tag:
                raise ValueError("Release or draft already exists; never duplicate an existing version")
    notes = f"""Candidate {version}, source `{revision}`.

Unsigned Windows x64 PostgreSQL and Redshift MSI candidates. Unit tests,
app-local packaged runtime loading, and read-only MSI structure checks passed.
Installer install/upgrade/rollback/uninstall and full live acceptance remain
pending. This draft is not a release approval or a claim of cross-platform
support. See `docs/RELEASING.md` before manual publication.
"""
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        assets = []
        for product in ("POSTGRESQL", "REDSHIFT"):
            # Transport envelope only: preserve all already-qualified MSI/evidence
            # bytes and filenames, so the included checksum inventory still works.
            target = out / f"odbcpp-{product.lower()}-{version}-windows-x64-candidate.tar.gz"
            with tarfile.open(target, "w:gz") as tar:
                for path in sorted((directory / product).iterdir()):
                    tar.add(path, arcname=path.name, recursive=False)
            checksum = target.with_suffix(target.suffix + ".sha256")
            checksum.write_text(f"{digest(target)}  {target.name}\n")
            assets.extend((str(target), str(checksum)))
        body = out / "NOTES.md"
        body.write_text(notes)
        subprocess.run(["gh", "release", "create", tag, "--draft", "--prerelease", "--target", revision,
                        "--title", "ODBCPP " + version, "--notes-file", str(body), *assets], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    draft(args.directory, args.revision)

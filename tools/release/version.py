#!/usr/bin/env python3
"""Derive every product version from version.json; no dependency or release I/O."""
import argparse
import json
import re
from pathlib import Path
import subprocess
from paths import member, regular, selected_root, write_generated


def derive(raw):
    if set(raw) != {"schemaVersion", "major", "minor", "patch", "prerelease", "sequence", "abi"}:
        raise ValueError("Unexpected version manifest fields")
    for name, maximum in (("schemaVersion", 1), ("major", 99), ("minor", 99), ("patch", 64), ("sequence", 499), ("abi", 65535)):
        if type(raw[name]) is not int or not 0 <= raw[name] <= maximum:
            raise ValueError(f"Invalid {name}")
    if raw["schemaVersion"] != 1 or raw["abi"] < 1:
        raise ValueError("Unsupported version schema or ABI")
    stage = raw["prerelease"]
    number = raw["sequence"]
    if stage not in ("", "beta", "rc") or (stage == "" and number != 0) or (stage and number == 0):
        raise ValueError("Expected beta.N, rc.N or stable with sequence zero")
    core = f'{raw["major"]}.{raw["minor"]}.{raw["patch"]}'
    semver = core + (f"-{stage}.{number}" if stage else "")
    build = raw["patch"] * 1000 + (number if stage == "beta" else 499 + number if stage == "rc" else 999)
    return {**raw, "core": core, "semver": semver,
            "msi": f'{raw["major"]}.{raw["minor"]}.{build}',
            "windows": f'{raw["major"]}.{raw["minor"]}.{build}.0',
            "odbc": f'{raw["major"]:02}.{raw["minor"]:02}.{raw["patch"]:04}'}


def metadata(source):
    source = selected_root(source)
    value = derive(json.loads(regular(member(source, "version.json")).read_text()))
    # Archives and vendored source exports remain buildable without Git. Never
    # discover a repository above this project and borrow its unrelated HEAD.
    value.update(sourceProvenance="export", sourceRevision=None,
                 trackedSourceClean=None, provenanceReason="no-repository-metadata")
    if (source / ".git").exists():
        def git(*args):
            return subprocess.check_output(["git", "-C", str(source), *args],
                                           text=True, stderr=subprocess.DEVNULL).strip()
        try:
            repository_root = Path(git("rev-parse", "--show-toplevel")).resolve()
            if repository_root != source:
                value["provenanceReason"] = "repository-root-mismatch"
            else:
                revision = git("rev-parse", "HEAD")
                if not re.fullmatch(r"[0-9a-f]{40}", revision):
                    raise ValueError("Unsupported Git revision identity")
                clean = not bool(git("status", "--porcelain", "--untracked-files=no"))
                value.update(sourceProvenance="git", sourceRevision=revision,
                             trackedSourceClean=clean, provenanceReason="exact-project-repository")
        except FileNotFoundError:
            value["provenanceReason"] = "git-unavailable"
        except (OSError, subprocess.CalledProcessError, ValueError):
            value["provenanceReason"] = "git-inspection-failed"
    if value["sourceProvenance"] == "git":
        suffix = "+g" + value["sourceRevision"][:12] + ("" if value["trackedSourceClean"] else ".dirty")
    else:
        suffix = "+export.unknown"
    value["display"] = value["semver"] + suffix
    return value


def generate(value, output):
    output.mkdir(parents=True, exist_ok=True)
    write_generated(member(output, "odbcpp-version.json"), json.dumps(value, indent=2) + "\n")
    cmake = {"CORE": value["core"], "SEMVER": value["semver"], "ODBC": value["odbc"], "ABI": value["abi"]}
    write_generated(member(output, "odbcpp-version.cmake"), "".join(f'set(ODBCPP_VERSION_{key} "{val}")\n' for key, val in cmake.items()))
    write_generated(member(output, "odbcpp-version.rc"), '''#include <windows.h>
1 VERSIONINFO
FILEVERSION %s
PRODUCTVERSION %s
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS %s
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_DLL
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "ODBCPP\\0"
   VALUE "FileDescription", "ODBCPP ODBC driver component\\0"
   VALUE "FileVersion", "%s\\0"
   VALUE "ProductVersion", "%s\\0"
   VALUE "ProductName", "ODBCPP\\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
''' % (value["windows"].replace(".", ","), value["windows"].replace(".", ","),
       "VS_FF_PRERELEASE" if value["prerelease"] else "0", value["windows"], value["display"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(metadata(args.source), args.output)

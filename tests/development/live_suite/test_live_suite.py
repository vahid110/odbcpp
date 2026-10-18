#!/usr/bin/env python3
"""Offline regression of CMake registration and actual CTest continuation.

This script is not a live runner or a live result parser. Its synthetic fixture
never opens a connection. Retains CTest's own artifacts in a fresh directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

options = argparse.ArgumentParser()
options.add_argument("--inventory-index", type=Path)
options.add_argument("--inventory-expected", type=Path)
args = options.parse_args()
assert bool(args.inventory_index) == bool(args.inventory_expected)
HERE = Path(__file__).resolve().parent
WORK = Path(tempfile.mkdtemp(prefix="odbcpp-ctest-offline-"))
os.umask(0o077)
fixture = HERE / "fixture.py"
cases = [dict(id=name.lower(), image="probe", profile="test", gtest_filter="Probe." + name,
              timeout_seconds=5) for name in ["Pass", "Fail", "Crash", "Skip", "Last", "Empty"]]
expected = {"schema_version": 1, "cases": [{key: row[key] for key in
             ["id", "image", "profile", "gtest_filter"]} for row in cases]}
profile_value = 'Driver={synthetic};Host=example.invalid;Password=synthetic\\;value;Quoted="literal"'
private = WORK / "private-environment.json"
private.write_text(json.dumps({"environment": ["TRACE_PATH=" + str(WORK / "trace.txt"),
                            "PROBE_PROFILE=" + profile_value]}))
index = {"schema_version": 1, "images": {"probe": {"executable": str(fixture),
         "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest()}},
         "profiles": {"test": {"environment_file": str(private)}}, "cases": cases}
index_path = WORK / "index.json"
expected_path = WORK / "expected.json"
index_path.write_text(json.dumps(index))
expected_path.write_text(json.dumps(expected))

def configure(directory, candidate=index, expected_input=expected_path):
    candidate_path = WORK / (directory + "-index.json")
    candidate_path.write_text(json.dumps(candidate))
    result = subprocess.run(["cmake", "-S", str(HERE), "-B", str(WORK / directory),
        "-DODBCPP_LIVE_INDEX=" + str(candidate_path),
        "-DODBCPP_LIVE_EXPECTED=" + str(expected_input)], capture_output=True, text=True)
    (WORK / (directory + "-configure.log")).write_text(result.stdout + result.stderr)
    return result

assert configure("run").returncode == 0
run = WORK / "run"
assert (run / "CTestTestfile.cmake").stat().st_mode & 0o777 == 0o600
assert run.stat().st_mode & 0o777 == 0o700
result = subprocess.run(["ctest", "--test-dir", str(run), "--parallel", "4",
    "--output-on-failure", "--output-junit", str(run / "ctest-results.xml")],
    capture_output=True, text=True)
(WORK / "ctest.log").write_text(result.stdout + result.stderr)
assert result.returncode != 0
trace = (WORK / "trace.txt").read_text().splitlines()
assert trace == ["Probe." + name + ":" + profile_value for name in
                 ["Pass", "Fail", "Crash", "Skip", "Last", "Empty"]], trace
results = ET.parse(run / "ctest-results.xml").getroot().findall("testcase")
assert len(results) == 6
assert results[0].find("failure") is None
assert results[1].find("failure") is not None
assert results[2].find("failure") is not None
# GoogleTest skip is read from its untouched standard XML. CTest exit0 is
# process success, not proof that the case body passed.
assert results[3].find("failure") is None
assert ET.parse(run / "gtest/case-4.xml").getroot().find(".//skipped") is not None
assert results[4].find("failure") is None
assert results[5].find("failure") is not None  # Wrong filter / zero test is FAIL.
assert len(list((run / "gtest").glob("*.xml"))) == 4  # Crash/empty create none.
failed = (run / "Testing/Temporary/LastTestsFailed.log").read_text()
assert "live.2.fail" in failed and "live.3.crash" in failed and "live.6.empty" in failed
assert (run / "Testing/Temporary/LastTest.log").exists()
for name, mutate in [
    ("missing", lambda j: j["cases"].pop()),
    ("extra", lambda j: j["cases"].append(dict(j["cases"][0]))),
    ("duplicate", lambda j: j["cases"].__setitem__(1, dict(j["cases"][0]))),
    ("reordered", lambda j: j["cases"].reverse()),
    ("bad-hash", lambda j: j["images"]["probe"].__setitem__("sha256", "0" * 64)),
    ("wildcard", lambda j: j["cases"][0].__setitem__("gtest_filter", "Probe.*")),
]:
    bad = json.loads(json.dumps(index))
    mutate(bad)
    assert configure(name, bad).returncode != 0, name
# Invalid private environment metadata must fail without exposing its values.
for name, environment in [("generator-expression", ["PROBE=$<1:secret>"]),
                          ("newline", ["PROBE=secret\nnext"]),
                          ("duplicate-variable", ["PROBE=one", "PROBE=two"])]:
    bad = json.loads(json.dumps(index))
    private_bad = WORK / (name + "-environment.json")
    private_bad.write_text(json.dumps({"environment": environment}))
    bad["profiles"]["test"]["environment_file"] = str(private_bad)
    rejected = configure(name, bad)
    assert rejected.returncode != 0, name
    assert "secret" not in rejected.stdout + rejected.stderr
full_count = None
if args.inventory_index:
    shadow = json.loads(args.inventory_index.read_text())
    for image in shadow["images"].values():
        image.update(executable=str(fixture), sha256=hashlib.sha256(fixture.read_bytes()).hexdigest())
    for profile in shadow["profiles"]:
        shadow["profiles"][profile] = {"environment": []}
    assert configure("full-inventory-registration", shadow, args.inventory_expected).returncode == 0
    listed = subprocess.run(["ctest", "--test-dir", str(WORK / "full-inventory-registration"), "-N"],
                            capture_output=True, text=True)
    (WORK / "full-inventory-list.log").write_text(listed.stdout + listed.stderr)
    full_count = len(shadow["cases"])
    assert listed.returncode == 0 and ("Total Tests: " + str(full_count)) in listed.stdout
print(json.dumps({"accepted": True, "actual_CTest_tests": 6, "failure_crash_and_zero_test_FAIL": True,
    "later_test_executed": True, "GoogleTest_skip_distinct_in_raw_XML": True, "private_semicolon_environment_preserved": True,
    "inventory_refusals": 6, "private_environment_refusals": 3,
    "full_inventory_registered_without_execution": full_count, "artifacts": str(WORK), "native_live_or_provider_operations": 0}))

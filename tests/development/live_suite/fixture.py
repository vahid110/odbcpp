#!/usr/bin/env python3
"""Offline CTest regression fixture only; never used to execute real live tests."""
import os
import sys
from pathlib import Path

case = next(arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--gtest_filter="))
output = next(arg.split("xml:", 1)[1] for arg in sys.argv if arg.startswith("--gtest_output="))
with open(os.environ["TRACE_PATH"], "a") as trace:
    trace.write(case + ":" + os.environ["PROBE_PROFILE"] + "\n")
if case == "Probe.Crash":
    print("[==========] Running 1 test from 1 test suite.", flush=True)
    os.abort()
if case == "Probe.Empty":
    print("[==========] Running 0 tests from 0 test suites.")
    sys.exit(0)
failed = case == "Probe.Fail"
skipped = case == "Probe.Skip"
Path(output).write_text(
    '<testsuites tests="1" failures="' + str(int(failed)) + '"><testsuite name="Probe">'
    '<testcase classname="Probe" name="' + case.split(".")[1] + '">'
    + ('<failure message="intentional"/>' if failed else '<skipped/>' if skipped else '')
    + '</testcase></testsuite></testsuites>')
print("[==========] Running 1 test from 1 test suite.")
if failed:
    print("[  SKIPPED ] diagnostic text is not a one-test SKIP summary")
if skipped:
    print("[  SKIPPED ] 1 test, listed below:")
else:
    print("[  FAILED ] " + case if failed else "[       OK ] " + case)
sys.exit(1 if failed else 0)

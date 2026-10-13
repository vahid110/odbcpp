#!/usr/bin/env python3
"""Run a local build command under the cooperative ODBCPP host build lease.

This schedules cooperating commands, not arbitrary processes. Never unlink the
lock file: waiters must continue using the same inode. It does not qualify a
candidate, enforce cloud admission, or establish descendant-process cleanup.
"""

import argparse
import fcntl
import math
import os
from pathlib import Path
import stat
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lock-file", type=Path,
        default=Path.home() / ".local/state/odbcpp/parallel-workflow/build.lock",
    )
    parser.add_argument("--wait-seconds", type=float, default=600)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("provide a command after --")
    if not math.isfinite(args.wait_seconds) or args.wait_seconds < 0:
        parser.error("--wait-seconds must be finite and nonnegative")
    args.lock_file.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(args.lock_file,
                 os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            parser.error("lock file must be a regular file")
        started = time.monotonic()
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                remaining = args.wait_seconds - (time.monotonic() - started)
                if remaining <= 0:
                    print("Build slot occupied; command was not started.", file=sys.stderr)
                    return 75
                time.sleep(min(0.1, remaining))
        print(f"Build slot acquired after {time.monotonic() - started:.2f}s.",
              file=sys.stderr, flush=True)
        # A child retains the lease if this wrapper is unexpectedly terminated.
        # Compatible descendants can retain it too; process cleanup is separate.
        try:
            result = subprocess.call(command, pass_fds=(fd,))
        except FileNotFoundError:
            print(f"Command not found: {command[0]}", file=sys.stderr)
            return 127
        return result if result >= 0 else 128 - result
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())

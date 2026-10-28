"""Private-file checks and the canonical, nonblocking local lock for live tooling.

The lock protects cooperating local callers only; it cannot constrain cloud use.
"""
from __future__ import annotations

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import stat
from typing import Iterator

CANONICAL_DIRECTORY = Path('/Users/vahidsbr/.local/state/odbcpp/redshift-pilot')
LOCK_NAME = 'pilot.lock'


class Blocked(Exception):
    """Only fixed public codes leave validation; no input or OS error text."""
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


def _need(condition: bool, code: str) -> None:
    if not condition:
        raise Blocked(code)


def _private_components(path: Path) -> None:
    """Reject symlink components; only the immediate private directory needs 0700."""
    _need(path.is_absolute(), 'unsafe_private_path')
    cursor = Path(path.anchor)
    for part in path.parts[1:]:
        _need(part not in ('.', '..'), 'unsafe_private_path')
        cursor = cursor / part
        info = os.lstat(cursor)
        _need(not stat.S_ISLNK(info.st_mode) and stat.S_ISDIR(info.st_mode), 'unsafe_private_path')
    info = os.lstat(path)
    _need(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700, 'unsafe_private_directory')


def _private_file(info) -> None:
    _need(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
          and stat.S_IMODE(info.st_mode) == 0o600 and info.st_nlink == 1, 'unsafe_private_file')


def validate_private_file(path) -> None:
    """Metadata inspection only: never open or read connection credentials."""
    try:
        _need(isinstance(path, str) and len(path) <= 4096, 'unsafe_private_path')
        value = Path(path)
        _private_components(value.parent)
        _private_file(os.lstat(value))
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('private_config_unavailable') from None


def assert_locked_directory(directory: Path, folder_fd: int) -> None:
    """Fail closed if the canonical path diverges from the borrowed directory."""
    try:
        directory = Path(directory)
        _private_components(directory)
        opened = os.fstat(folder_fd)
        current = os.stat(directory, follow_symlinks=False)
        _need(stat.S_ISDIR(opened.st_mode) and opened.st_uid == os.getuid()
              and stat.S_IMODE(opened.st_mode) == 0o700, 'unsafe_private_directory')
        _need((opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino), 'directory_identity_changed')
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('directory_identity_changed') from None


@contextmanager
def pilot_lock(directory: Path = CANONICAL_DIRECTORY) -> Iterator[int]:
    """Nonblocking stable lock inode; never delete, truncate or stale-reap it.

    Tests pass a private temporary directory. Production defaults to the single
    canonical path, independent of cwd/worktree. Directory must already exist.
    Yields the exact verified directory fd, borrowed only until context exit.
    Consumers must not close/retain it, reopen the pathname, or unlink the lock.
    Consumers ignoring the yielded value remain compatible. Use
    assert_locked_directory before/after directory-relative state operations.
    """
    folder_fd = lock_fd = None
    try:
        directory = Path(directory)
        _private_components(directory)
        folder_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        folder_info = os.fstat(folder_fd)
        _need(folder_info.st_uid == os.getuid() and stat.S_IMODE(folder_info.st_mode) == 0o700,
              'unsafe_private_directory')
        assert_locked_directory(directory, folder_fd)
        flags = os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
        try:
            lock_fd = os.open(LOCK_NAME, flags | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=folder_fd)
        except FileExistsError:
            lock_fd = os.open(LOCK_NAME, flags, dir_fd=folder_fd)
        _private_file(os.fstat(lock_fd))
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Blocked('lock_contended') from None
        current = os.stat(LOCK_NAME, dir_fd=folder_fd, follow_symlinks=False)
        _need((current.st_dev, current.st_ino) == (os.fstat(lock_fd).st_dev, os.fstat(lock_fd).st_ino),
              'lock_identity_changed')
        assert_locked_directory(directory, folder_fd)
        yield folder_fd
        assert_locked_directory(directory, folder_fd)
        _private_file(os.fstat(lock_fd))
        current = os.stat(LOCK_NAME, dir_fd=folder_fd, follow_symlinks=False)
        _need((current.st_dev, current.st_ino) == (os.fstat(lock_fd).st_dev, os.fstat(lock_fd).st_ino),
              'lock_identity_changed')
    except Blocked:
        raise
    except (OSError, ValueError, TypeError):
        raise Blocked('lock_unavailable') from None
    finally:
        # Closing descriptor releases OS lock; retain stable file for all chats.
        if lock_fd is not None:
            os.close(lock_fd)
        if folder_fd is not None:
            os.close(folder_fd)

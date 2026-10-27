#!/usr/bin/env python3
"""Verify this one derived AWS SDK source closure; no build/prefix admission.

Read-only Git metadata checks. Never fetches, checks out, installs, builds, loads
SDK code or discovers credentials. Build outside the source tree; even ignored
untracked source-tree files are refused. Keep the original SDK trees intact.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import time

LOCK = Path(__file__).resolve().parents[3] / 'cmake/dependencies/aws-sdk-1.11.906-mqtt-security.json'
SHA = re.compile(r'[0-9a-f]{40}')
STATUS = re.compile(r'([ +\-U])([0-9a-f]{40}) ([A-Za-z0-9_./-]+)(?: \([^\r\n]*\))?')


class SourceError(ValueError):
    pass


def relative_path(value):
    if not isinstance(value, str):
        raise SourceError('invalid source-lock submodule path')
    path = PurePosixPath(value)
    if (path.is_absolute() or not value
            or any(part in ('', '.', '..', '.git') for part in value.split('/'))
            or re.fullmatch(r'[A-Za-z0-9_./-]+', value) is None):
        raise SourceError('invalid source-lock submodule path')
    return value


def load_lock():
    with LOCK.open('rb') as stream:
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise SourceError('oversized source lock')
    lock = json.loads(raw)
    if (not isinstance(lock, dict) or lock.get('schema_version') != 1
            or lock.get('sdk_version') != '1.11.906'
            or not isinstance(lock.get('sdk_commit'), str)
            or not SHA.fullmatch(lock['sdk_commit'])):
        raise SourceError('unsupported source lock')
    modules = lock.get('submodules')
    if not isinstance(modules, dict) or not 2 <= len(modules) <= 32:
        raise SourceError('invalid source-lock closure')
    for path, commit in modules.items():
        relative_path(path)
        if not isinstance(commit, str) or not SHA.fullmatch(commit):
            raise SourceError('invalid source-lock commit')
    override = lock.get('override', {})
    if (not isinstance(override, dict) or not isinstance(override.get('path'), str)
            or override.get('path') not in modules
            or modules[override['path']] != override.get('fixed_commit')
            or not isinstance(override.get('baseline_commit'), str)
            or not SHA.fullmatch(override['baseline_commit'])
            or override.get('baseline_commit') == override.get('fixed_commit')):
        raise SourceError('invalid sole MQTT override')
    return lock


class Git:
    def __init__(self):
        self.deadline = time.monotonic() + 60

    def __call__(self, root, *arguments, allow_one=False):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise SourceError('source verification deadline')
        try:
            result = subprocess.run(
                ['git', '--no-optional-locks', '-c', 'core.fsmonitor=false', '-C', str(root), *arguments],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=min(10, remaining), check=False)
        except (OSError, subprocess.TimeoutExpired):
            raise SourceError('Git source verification unavailable') from None
        if len(result.stdout) > 1024 * 1024:
            raise SourceError('oversized Git metadata')
        if result.returncode not in ((0, 1) if allow_one else (0,)):
            raise SourceError('Git source verification failed')
        return result.returncode, result.stdout.decode('utf-8', errors='strict')


def verify_source(root, lock, git=None):
    git = git or Git()
    root = Path(root).resolve(strict=True)
    expected = {'.': lock['sdk_commit'], **lock['submodules']}
    override = lock['override']['path']
    # Fail old/reset MQTT early, before enumerating an existing in-tree build.
    for path in ('.', 'crt/aws-crt-cpp', override):
        if path not in expected:
            raise SourceError('incomplete source lock')
        directory = (root / path).resolve(strict=True)
        try:
            directory.relative_to(root)
        except ValueError:
            raise SourceError('submodule escapes source root') from None
        _, head = git(directory, 'rev-parse', 'HEAD')
        if head.strip() != expected[path]:
            raise SourceError('source HEAD differs from lock: ' + path)
    _, output = git(root, 'submodule', 'status', '--recursive')
    observed = {}
    for line in output.splitlines():
        match = STATUS.fullmatch(line)
        if match is None:
            raise SourceError('malformed submodule inventory')
        flag, commit, path = match.groups()
        relative_path(path)
        if path in observed or path not in lock['submodules']:
            raise SourceError('duplicate or unexpected submodule')
        if commit != expected[path] or flag != ('+' if path == override else ' '):
            raise SourceError('old/reset/uninitialized or additional submodule drift: ' + path)
        observed[path] = commit
    if observed != lock['submodules']:
        raise SourceError('missing source submodule')
    for path, commit in expected.items():
        directory = (root / path).resolve(strict=True)
        try:
            directory.relative_to(root)
        except ValueError:
            raise SourceError('submodule escapes source root') from None
        _, top = git(directory, 'rev-parse', '--show-toplevel')
        _, head = git(directory, 'rev-parse', 'HEAD')
        if Path(top.strip()).resolve() != directory or head.strip() != commit:
            raise SourceError('source identity changed during verification: ' + path)
        changed, _ = git(directory, 'diff', '--quiet', '--no-ext-diff', '--no-textconv',
                         '--ignore-submodules=all', 'HEAD', '--', allow_one=True)
        staged, _ = git(directory, 'diff', '--cached', '--quiet', '--no-ext-diff', '--no-textconv',
                        '--ignore-submodules=none', allow_one=True)
        _, untracked = git(directory, 'ls-files', '--others', '--directory', '-z')
        if changed or staged or untracked:
            raise SourceError('dirty/staged/untracked source contents: ' + path)
    return {'schema_version': 1, 'sdk_version': lock['sdk_version'],
            'source_heads': expected, 'sole_gitlink_override': override,
            'source_closure_verified': True, 'installed_prefix_qualified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = verify_source(args.source_root, load_lock())
    except (SourceError, OSError, UnicodeError, json.JSONDecodeError) as error:
        parser.error(str(error))
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()

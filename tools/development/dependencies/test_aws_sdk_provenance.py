"""Offline source-lock controls; no SDK checkout/build/prefix/provider actions."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('aws_source_lock', Path(__file__).with_name('aws_sdk_provenance.py'))
source = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(source)


class FixtureGit:
    def __init__(self, root, lock):
        self.root = root
        self.lock = lock
        self.heads = {'.': lock['sdk_commit'], **lock['submodules']}
        self.lines = [('+' if path == lock['override']['path'] else ' ') + commit + ' ' + path
                      for path, commit in lock['submodules'].items()]
        self.changed = self.staged = False
        self.untracked = ''
        self.calls = []

    def __call__(self, root, *args, allow_one=False):
        self.calls.append((root, args))
        path = root.relative_to(self.root).as_posix()
        if args == ('rev-parse', 'HEAD'):
            return 0, self.heads[path] + '\n'
        if args == ('rev-parse', '--show-toplevel'):
            return 0, str(root) + '\n'
        if args == ('submodule', 'status', '--recursive'):
            return 0, '\n'.join(self.lines) + '\n'
        if args[0] == 'diff':
            return int(self.staged if '--cached' in args else self.changed), ''
        if args[0] == 'ls-files':
            self.last_untracked_args = args
            return 0, self.untracked
        raise AssertionError(args)


class SourceClosureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve() / 'source'
        self.root.mkdir()
        self.lock = source.load_lock()
        for path in self.lock['submodules']:
            (self.root / path).mkdir(parents=True, exist_ok=True)
        self.git = FixtureGit(self.root, self.lock)

    def verify(self):
        return source.verify_source(self.root, self.lock, self.git)

    def test_exact_fixed_source_passes_without_binary_qualification(self):
        before = sorted(str(p.relative_to(self.root)) for p in self.root.rglob('*'))
        result = self.verify()
        self.assertTrue(result['source_closure_verified'])
        self.assertFalse(result['installed_prefix_qualified'])
        self.assertEqual(len(result['source_heads']), 16)
        self.assertEqual(result['source_heads'][self.lock['override']['path']], self.lock['override']['fixed_commit'])
        self.assertEqual(before, sorted(str(p.relative_to(self.root)) for p in self.root.rglob('*')))

    def test_old_or_reset_mqtt_and_wrong_sdk_or_crt_head_fail(self):
        for path in ('.', 'crt/aws-crt-cpp', self.lock['override']['path']):
            with self.subTest(path=path):
                original = self.git.heads[path]
                self.git.heads[path] = self.lock['override']['baseline_commit']
                with self.assertRaisesRegex(source.SourceError, 'HEAD differs'):
                    self.verify()
                self.git.heads[path] = original

    def test_no_extra_missing_duplicate_or_uninitialized_submodule(self):
        original = list(self.git.lines)
        for lines in (original[:-1], original + [original[-1]],
                      original + [' ' + 'a' * 40 + ' extra'],
                      ['-' + original[0][1:]] + original[1:],
                      ['U' + original[0][1:]] + original[1:]):
            with self.subTest(lines=lines):
                self.git.lines = lines
                with self.assertRaises(source.SourceError):
                    self.verify()

    def test_only_locked_mqtt_may_differ_from_parent_gitlink(self):
        original = list(self.git.lines)
        for index, line in enumerate(original):
            if line[0] == '+':
                continue
            self.git.lines = original[:index] + ['+' + line[1:]] + original[index + 1:]
            with self.subTest(path=line[42:]), self.assertRaises(source.SourceError):
                self.verify()

    def test_fixed_mqtt_staged_into_parent_is_not_allowed(self):
        self.git.lines = [' ' + line[1:] if line[0] == '+' else line for line in self.git.lines]
        with self.assertRaises(source.SourceError):
            self.verify()

    def test_dirty_staged_or_even_ignored_untracked_inputs_fail(self):
        for field, value in [('changed', True), ('staged', True), ('untracked', 'ignored-build/\0')]:
            with self.subTest(field=field):
                setattr(self.git, field, value)
                with self.assertRaisesRegex(source.SourceError, 'dirty/staged/untracked'):
                    self.verify()
                setattr(self.git, field, False if field != 'untracked' else '')
        self.verify()
        self.assertNotIn('--exclude-standard', self.git.last_untracked_args)

    def test_symlink_escape_rejected_before_git_runs_outside(self):
        mqtt = self.root / self.lock['override']['path']
        mqtt.rmdir()
        outside = self.root.parent / 'outside'
        outside.mkdir()
        try:
            mqtt.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest('directory symlinks unavailable: ' + str(error))
        with self.assertRaisesRegex(source.SourceError, 'escapes'):
            self.verify()
        self.assertNotIn(outside, [directory for directory, _ in self.git.calls])

    def test_malformed_inventory_and_path_injection_fail(self):
        for line in ('not a submodule', ' ' + 'a' * 40 + ' ../escape',
                     ' ' + 'a' * 40 + ' /absolute', ' ' + 'a' * 40 + ' x;command'):
            self.git.lines = [line]
            with self.subTest(line=line), self.assertRaises(source.SourceError):
                self.verify()

    def test_a_later_head_change_is_not_hidden_by_status_snapshot(self):
        original = self.git.__call__
        reads = 0
        def changing(root, *args, **kwargs):
            nonlocal reads
            if args == ('rev-parse', 'HEAD'):
                reads += 1
                if reads > 3:
                    return 0, 'f' * 40
            return original(root, *args, **kwargs)
        with self.assertRaisesRegex(source.SourceError, 'identity changed'):
            source.verify_source(self.root, self.lock, changing)

    def test_other_submodule_head_cannot_hide_behind_correct_status(self):
        path = 'crt/aws-crt-cpp/crt/aws-c-auth'
        self.git.heads[path] = 'f' * 40
        with self.assertRaisesRegex(source.SourceError, 'identity changed'):
            self.verify()

    def test_lock_rejects_corrupt_schema_commit_path_and_override(self):
        for change in (lambda x: x.update(schema_version=2),
                       lambda x: x.update(sdk_commit='main'),
                       lambda x: x['submodules'].update({'../outside': 'a' * 40}),
                       lambda x: x['override'].update(path=[]),
                       lambda x: x['override'].update(fixed_commit='a' * 40)):
            lock = deepcopy(self.lock); change(lock)
            path = self.root.parent / 'lock.json'; path.write_text(json.dumps(lock))
            with patch.object(source, 'LOCK', path), self.assertRaises(source.SourceError):
                source.load_lock()

    def test_git_wrapper_never_uses_shell_fetch_or_optional_index_writes(self):
        result = subprocess.CompletedProcess([], 0, b'public metadata\n')
        with patch.object(source.subprocess, 'run', return_value=result) as run:
            code, text = source.Git()(self.root, 'rev-parse', 'HEAD')
        self.assertEqual((code, text), (0, 'public metadata\n'))
        args, kwargs = run.call_args
        self.assertIn('--no-optional-locks', args[0])
        self.assertIn('core.fsmonitor=false', args[0])
        self.assertNotIn('shell', kwargs)
        self.assertNotIn('fetch', args[0])
        with patch.object(source.subprocess, 'run', side_effect=subprocess.TimeoutExpired('git', 10)):
            with self.assertRaises(source.SourceError):
                source.Git()(self.root, 'rev-parse', 'HEAD')


if __name__ == '__main__':
    unittest.main()

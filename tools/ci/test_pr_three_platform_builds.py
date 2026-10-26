"""Offline PR scheduling/build safety; never runs CMake, CTest or a provider API.

The narrow workflow readers below cover the repository's literal job layout.
YAML syntax validation is separate; these checks reject event/profile changes that
would silently drop a platform or turn a PR build into live/full acceptance.
"""
import ast
import hashlib
import itertools
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PR_JOBS = ('pr-ubuntu-build', 'pr-windows-build')
FULL_JOBS = ('build-and-test', 'redshift-build', 'windows-live', 'windows-package',
             'linux-sanitizers', 'linux-openssl-static', 'awslc-adapter-proof',
             'iodbc-unicode')
FLAGS = ('run_development_pilot', 'run_product_qualification',
         'run_windows_product_qualification', 'run_sanitizer_only', 'run_linked_server')
# Reviewed job bodies, excluding only their top-level event condition. Preserve
# 526b with reviewed packaging/checkpoint repairs (2026-10-22), then action pins
# and literal cache-token grants (2026-10-25), then the reviewed exact PR109
# checkout v7.0.1 pins; build/test commands are unchanged.
BODY_SHA256 = {
    'sanitizer-only': 'b09a63618df8499a9ccb181cc85f46065038f65faa72b86ee084e7ea9995cd12',
    'ubuntu-development-pilot': '5b8a297a03e73c2d015782faed0336f5f49657c32df2071f42a21e2faf5e5ea8',
    'ubuntu-product-qualification': '8a4dd75f8c81679ccd0a5315c0e1dab5b34d9b74d30700d46494f20e077be46d',
    'windows-product-qualification': '5d0e12ab292bd0a2a60cb15de3cecef5fca9ff93d4a5265601fd29693d91c907',
    'build-and-test': 'b02b5d0d4c3c417c230756fadfde79f7b745c3538c3ba5cc7cd05bafac509c69',
    'redshift-build': '1ebe8532cedef3cbc57003e5e603ae70bc5c6642ae38e1ddd35c6ce1bfad1f56',
    'windows-live': '0030e81243013771fdb37fef1a57e25f3a14fe88509ab375118154028c6909d0',
    'windows-package': 'b08bb294b5fa5a22b0b5af8ed1f1a4f0a071b158883560d421956e7c77497e18',
    'windows-linked-server': '78a55ae6ddd98acba64baf3d5e1b401cc3e5480bb58d8906667ad7e4c2df8a9d',
    'linux-sanitizers': 'd9eb380226d38c7e70f2730b22b550cb12c41c7627f10b5427b725a03bb75a3a',
    'linux-openssl-static': 'c629a493344201b61152fcbda4d4abea427e4f6064333d753143b38a95bf396e',
    'awslc-adapter-proof': 'cd7dad823fd9582ab5990ff6286ea6932311babcbc07840198b3eeb044c63d3a',
    'iodbc-unicode': '4bd25959794f57d60880ae151748406b555451b29e14d03da40e2c90b7dab527',
}


def jobs(text):
    if text.count('\njobs:\n') != 1:
        raise ValueError('one literal jobs mapping required')
    pairs = re.findall(r'^  ([a-z][a-z0-9-]*):\n(.*?)(?=^  [a-z][a-z0-9-]*:|\Z)',
                       text.split('\njobs:\n', 1)[1], re.M | re.S)
    if len(pairs) != len(dict(pairs)):
        raise ValueError('duplicate job key')
    return dict(pairs)


def enabled(block, event, flags):
    match = re.search(r'^    if: (.*)$', block, re.M)
    if not match:
        return True
    expression = match[1].replace('${{', '').replace('}}', '').strip()
    expression = expression.replace('github.event_name', repr(event))
    expression = re.sub(r'inputs\.([a-z_]+)', lambda m: repr(flags.get(m[1], False)), expression)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression).strip()
    tree = ast.parse(expression, mode='eval')
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
               ast.Compare, ast.Eq, ast.NotEq, ast.Constant, ast.Load)
    if not all(isinstance(n, allowed) for n in ast.walk(tree)):
        raise ValueError('unsupported event predicate')
    return bool(eval(compile(tree, '<workflow boolean>', 'eval'), {'__builtins__': {}}, {}))


def unrestricted_pr_trigger(text):
    match = re.search(r'^  pull_request:[ \t]*\n(.*?)(?=^  [a-z_]+:|^\S|\Z)', text, re.M | re.S)
    if not match or any(line.strip() and not line.lstrip().startswith('#')
                        for line in match[1].splitlines()):
        raise ValueError('PR trigger must include all bases, paths and draft PRs')


def build_contract(block, platform):
    required = ["if: github.event_name == 'pull_request'", 'timeout-minutes: 30',
                'ref: ${{ github.sha }}', 'persist-credentials: false',
                'git log -1 --format="%H %T"', '-DBUILD_TESTING=ON',
                '-DBUILD_EXAMPLES=OFF', '-DTARGET_DATABASE=POSTGRESQL',
                '-DODBCPP_CRYPTO_PROVIDER=OPENSSL', '-DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF',
                '-DODBCPP_CRYPTO_TLS_PROOF=OFF', '-DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2',
                '-DODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2']
    if platform == 'Ubuntu':
        required += ['runs-on: ubuntu-latest', '-DCMAKE_BUILD_TYPE=Release',
                     '-DODBC_DRIVER_MANAGER_FLAVOR=UNIXODBC',
                     '-DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED',
                     'cmake --build build-pr-ubuntu --parallel 2']
    else:
        required += ['runs-on: windows-2025', '-A x64',
                     '-DODBCPP_CRYPTO_LINKAGE=BUNDLED_SHARED',
                     '-DODBCPP_CRYPTO_ROOT="C:/Program Files/OpenSSL"',
                     'cmake --build build-pr-windows --config Release --parallel 2']
    if any(value not in block for value in required):
        raise ValueError('missing exact PR build profile/source/bound')
    if len(re.findall(r'^    if:', block, re.M)) != 1 or re.search(r'^      - .*\n\s+if:', block):
        raise ValueError('build must not be conditional on draft/path/status')
    for forbidden in ['services:', 'container:', 'secrets.', 'secrets:', 'self-hosted',
                      'actions/cache', 'restore-keys:', 'ctest', '--gtest_', '--target',
                      'windows-postgres', 'windows-package', 'reg.exe', 'msiexec',
                      'continue-on-error:', 'needs:', 'inputs.', 'github.event.pull_request.draft']:
        if forbidden in block:
            raise ValueError('PR build cannot restore binaries, skip work, or run tests/live/installers')
    if re.search(r'\b(?:docker|aws|psql)\s', block):
        raise ValueError('no services, paid or SQL operation')
    build_lines = [line.strip() for line in block.splitlines() if 'cmake --build ' in line]
    if len(build_lines) != 1 or not build_lines[0].endswith('--parallel 2'):
        raise ValueError('one default ALL build j2 required')


def cache_mode(text, job=False):
    """Read the literal token grant; no hosted token behavior is simulated."""
    pattern = r'^    cache-mode: ([a-z-]+)$' if job else r'^cache-mode: ([a-z-]+)$'
    values = re.findall(pattern, text, re.M)
    if len(values) > 1:
        raise ValueError('duplicate cache authority')
    return values[0] if values else None


def candidate_cache_contract(ci, ubuntu, windows):
    """Fail closed on candidate write grants or mutable producer identity."""
    blocks = jobs(ubuntu)
    inherited = cache_mode(ubuntu)
    if inherited != 'read' or cache_mode(blocks['product'], job=True) != 'read':
        raise ValueError('arbitrary product candidates require a read-only cache token')
    writers = {name for name, block in blocks.items()
               if (cache_mode(block, job=True) or inherited) in ('write', 'write-only')}
    if writers != {'cold', 'seed-a'}:
        raise ValueError('only reviewed source producers may write caches')
    if ('ref: ${{ github.sha }}' not in blocks['cold'] or
            blocks['cold'].count('uses: actions/checkout@') != 1):
        raise ValueError('same-head producer must execute only the dispatch source')
    if 'PILOT_EXPECTED_HEAD: cac4e780dcc9ce84fcc8a3d104ab1afbf4ba0547' not in blocks['seed-a']:
        raise ValueError('seed producer checkout must use the literal reviewed source')
    if 'ref: ${{ env.PILOT_EXPECTED_HEAD }}' not in blocks['seed-a']:
        raise ValueError('seed checkout must use its fixed source identity')
    if 'uses: actions/cache/save@' in blocks['product']:
        raise ValueError('candidate job must not advertise a cache producer')
    ci_blocks = jobs(ci)
    if (cache_mode(ci_blocks['sanitizer-only'], job=True) != 'none' or
            cache_mode(ci_blocks['windows-product-qualification'], job=True) != 'none' or
            cache_mode(windows) != 'none'):
        raise ValueError('cold sanitizer/Windows candidates require no cache access')
    for block, check in [(blocks['product'], 'run: test "$ACTIONS_CACHE_MODE" = read'),
                         (ci_blocks['sanitizer-only'], 'run: test "$ACTIONS_CACHE_MODE" = none'),
                         (jobs(windows)['product'], "if ($env:ACTIONS_CACHE_MODE -ne 'none')")]:
        if check not in block or block.index(check) > block.index('uses: actions/checkout@'):
            raise ValueError('require the runner-reported cache grant before candidate checkout')


class CITrustBoundaries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ci = (ROOT / '.github/workflows/ci.yml').read_text()
        cls.ubuntu = (ROOT / '.github/workflows/ubuntu-development-pilot.yml').read_text()
        cls.windows = (ROOT / '.github/workflows/windows-product-qualification.yml').read_text()

    def test_candidate_token_caps_and_safe_producers(self):
        candidate_cache_contract(self.ci, self.ubuntu, self.windows)
        for filename in ['primary.yml', 'scorecard.yml']:
            self.assertEqual(cache_mode((ROOT / '.github/workflows' / filename).read_text()), 'none')

    def test_candidate_write_or_mutable_seed_is_rejected(self):
        mutations = [self.ubuntu.replace('cache-mode: read', 'cache-mode: write', 1),
                     self.ubuntu.replace('  product:\n    cache-mode: read', '  product:\n    cache-mode: write'),
                     self.ubuntu.replace('PILOT_EXPECTED_HEAD: cac4e780dcc9ce84fcc8a3d104ab1afbf4ba0547',
                                         'PILOT_EXPECTED_HEAD: ${{ inputs.seed_head }}'),
                     self.ubuntu.replace('  warm:\n', '  warm:\n    cache-mode: write\n')]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                candidate_cache_contract(self.ci, mutation, self.windows)
        with self.assertRaises(ValueError):
            candidate_cache_contract(self.ci.replace('cache-mode: none', 'cache-mode: write', 1),
                                     self.ubuntu, self.windows)

    def test_all_external_actions_use_full_commit_pins(self):
        for path in (ROOT / '.github/workflows').glob('*.yml'):
            for action in re.findall(r'^\s+(?:- )?uses: ([^\s#]+)', path.read_text(), re.M):
                if action.startswith('./'):
                    continue
                with self.subTest(path=path.name, action=action):
                    self.assertRegex(action, r'^[\w.-]+/[\w./-]+@[0-9a-f]{40}$')


class ThreePlatformScheduling(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ci = (ROOT / '.github/workflows/ci.yml').read_text()
        cls.primary = (ROOT / '.github/workflows/primary.yml').read_text()
        cls.blocks = jobs(cls.ci)

    def test_all_PR_paths_bases_and_drafts_trigger(self):
        unrestricted_pr_trigger(self.ci)
        unrestricted_pr_trigger(self.primary)

    def test_PR_only_runs_two_new_builds_for_every_input_combination(self):
        for bits in itertools.product([False, True], repeat=len(FLAGS)):
            flags = dict(zip(FLAGS, bits))
            with self.subTest(flags=flags):
                self.assertEqual({n for n,b in self.blocks.items() if enabled(b,'pull_request',flags)}, set(PR_JOBS))

    def test_full_dispatch_keeps_ten_mandatory_and_seven_skipped(self):
        active = {n for n,b in self.blocks.items() if enabled(b,'workflow_dispatch',{})}
        self.assertEqual(active, set(FULL_JOBS))
        self.assertEqual(len(self.blocks), 15)
        self.assertEqual(sum(2 if n in ['awslc-adapter-proof','iodbc-unicode'] else 1 for n in active), 10)
        self.assertEqual(len(self.blocks)-len(active), 7)

    def test_manual_focused_mode_truth_table_preserved(self):
        for bits in itertools.product([False, True], repeat=len(FLAGS)):
            f = dict(zip(FLAGS, bits))
            active = {n for n,b in self.blocks.items() if enabled(b,'workflow_dispatch',f)}
            expected = set()
            if f['run_sanitizer_only']:
                expected.add('sanitizer-only')
            elif f['run_windows_product_qualification']:
                expected.add('windows-product-qualification')
            else:
                if f['run_product_qualification']:
                    expected.add('ubuntu-product-qualification')
                elif f['run_development_pilot']:
                    expected.add('ubuntu-development-pilot')
                if not any(f[k] for k in FLAGS[:4]):
                    expected.update(FULL_JOBS)
                    if f['run_linked_server']:
                        expected.add('windows-linked-server')
            with self.subTest(flags=f):
                self.assertEqual(active, expected)

    def test_checkpoint_and_focused_job_bodies_match_reviewed_pins(self):
        for name, expected in BODY_SHA256.items():
            body = re.sub(r'^    if: .*\n', '', self.blocks[name], flags=re.M).strip()
            self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), expected, name)

    def test_installer_gate_runs_in_existing_elevated_CI_before_legacy_fixtures(self):
        block = self.blocks['windows-package']
        self.assertIn('needs: windows-live', block)
        self.assertLess(block.index('windows-product-packages.ps1'),
                        block.index('./tools/ci/windows-package.ps1'))
        self.assertIn('- name: Validate fresh-runner package lifecycle\n        if: always()', block)
        self.assertIn('./tools/ci/windows-product-packages.ps1 -VerifyCleanup', block)
        self.assertIn('${{ runner.temp }}/dual-product-package-evidence', block)
        script = (ROOT/'tools/ci/windows-product-packages.ps1').read_text()
        self.assertIn('WindowsBuiltInRole]::Administrator', script)
        self.assertIn('-PostgreSqlSetupBuild $setupPg -RedshiftSetupBuild $setupRs', script)
        self.assertIn('packaging/windows/test-product-lifecycle.ps1', script)
        self.assertNotIn('-Verb RunAs', script)
        self.assertNotIn('Register-ScheduledTask', script)

    def test_macOS_primary_retains_actual_default_ALL_build(self):
        block = jobs(self.primary)['macos-iodbc-ucs4']
        self.assertIn('runs-on: macos-latest', block)
        self.assertNotRegex(block, r'(?m)^    if:')
        self.assertIn('cmake --build build-iodbc --parallel 2', block)
        self.assertIn('-DTARGET_DATABASE=POSTGRESQL', block)
        self.assertIn('-DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=4', block)

    def test_two_build_profiles_are_cold_default_ALL_without_runtime(self):
        for name, platform in zip(PR_JOBS, ['Ubuntu','Windows']):
            with self.subTest(platform=platform):
                build_contract(self.blocks[name], platform)

    def test_primary_PR_and_explicit_checkpoint_without_duplicate_push(self):
        unrestricted_pr_trigger(self.primary)
        triggers = self.primary.split('\npermissions:', 1)[0]
        self.assertIn('  workflow_dispatch:', triggers)
        self.assertNotRegex(triggers, r'(?m)^  push:')
        # Job names/builds and required PR checks remain unchanged. This does
        # not grant cross-run artifact reuse or bypass a branch-required check.

    def test_mutated_PR_event_filters_are_refused(self):
        for filter_line in ['branches-ignore: [main]', 'paths: ["**.cpp"]',
                            'types: [ready_for_review]', 'branches: [main]']:
            text = self.ci.replace('  pull_request:\n', '  pull_request:\n    '+filter_line+'\n', 1)
            with self.subTest(filter=filter_line), self.assertRaises(ValueError):
                unrestricted_pr_trigger(text)

    def test_duplicate_job_key_and_malformed_jobs_mapping_refused(self):
        for text in [self.ci+'\n  build-and-test:\n    runs-on: ubuntu-latest\n',
                     self.ci.replace('\njobs:\n','\n jobs:\n',1)]:
            with self.assertRaises(ValueError):
                jobs(text)

    def test_unknown_predicate_cannot_execute(self):
        with self.assertRaises(ValueError):
            enabled('    if: __import__("os").system("false")\n', 'pull_request', {})

    def test_each_profile_guard_rejects_meaningful_negative_mutations(self):
        for name, platform in zip(PR_JOBS, ['Ubuntu','Windows']):
            base = self.blocks[name]
            mutations = [base.replace('--parallel 2','--parallel 4'),
                         base.replace('--parallel 2','--target test_backend_contract --parallel 2'),
                         base.replace('timeout-minutes: 30','timeout-minutes: 90'),
                         base.replace('-DBUILD_TESTING=ON','-DBUILD_TESTING=OFF'),
                         base.replace('ref: ${{ github.sha }}','ref: main'),
                         base.replace('-DODBCPP_CRYPTO_TLS_PROOF=OFF','-DODBCPP_CRYPTO_TLS_PROOF=ON'),
                         base.replace("if: github.event_name == 'pull_request'", "if: github.event_name == 'pull_request' && !github.event.pull_request.draft")]
            mutations += [base+'\n    '+x+'\n' for x in ['services: {}','needs: other-job','continue-on-error: true','secrets: inherit','container: paid-image','restore-keys: binary','run: ctest --test-dir build','run: docker run postgres','run: aws sts get-caller-identity']]
            for mutant in mutations:
                with self.subTest(job=name,mutation=mutations.index(mutant)), self.assertRaises(ValueError):
                    build_contract(mutant, platform)


if __name__ == '__main__':
    unittest.main()

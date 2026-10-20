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
# Exact 526b job bodies, excluding only their top-level event condition.
BODY_SHA256 = {
    'sanitizer-only': 'fbc8b8a9ef873d8d1b13f429b3110338f360adf941936543f1a0df31e71909b7',
    'ubuntu-development-pilot': '5b8a297a03e73c2d015782faed0336f5f49657c32df2071f42a21e2faf5e5ea8',
    'ubuntu-product-qualification': '8a4dd75f8c81679ccd0a5315c0e1dab5b34d9b74d30700d46494f20e077be46d',
    'windows-product-qualification': '6bffeb8c5f8356015036a032d0f37128180fbf8c011084bbfd208ce1a40e80e6',
    'build-and-test': 'a5f64cdc03b610e37ca827a9c189430effc5d21655f5b4d5fc7e13790dc77c3f',
    'redshift-build': '83dd943e7dad86a66e4c44124163bbd612194b914c19abba7fd702d218787048',
    'windows-live': '79bb1a40d5fa0a94938f1b4fa4a4165326919139b7d88387df0e15f35cca0517',
    'windows-package': '265b46a0c7fcd047c3ac7e5b93a23485fef66d7afb6f637411eb158a459571e3',
    'windows-linked-server': 'f7f15d6261db38ead9993bb28ea275523b1af5835780e1c28ed075844f536c0d',
    'linux-sanitizers': 'e302e94886a90342a0abf1b386d46afb5b36d13ea49c6802034ffbc12188d2ab',
    'linux-openssl-static': '04a0418d777e90f5fd04e9a1fcca6fdec7c4731630d11c765825ab29abd1d930',
    'awslc-adapter-proof': '19230fd79ff37164624bd764631ad194679a7ef63c3aaf3594d5d4826970969d',
    'iodbc-unicode': '6ea54a59df2efc5a4002e8ddf9b9b0f68f4216fdd60b94f0cb3867dac3a21a37',
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

    def test_existing_checkpoint_and_focused_job_bodies_byte_preserved(self):
        for name, expected in BODY_SHA256.items():
            body = re.sub(r'^    if: .*\n', '', self.blocks[name], flags=re.M).strip()
            self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), expected, name)

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

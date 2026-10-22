"""Prepare the closed Ubuntu static OpenSSL CI inputs; no compilation or install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess


ARCHIVES = {
    'ssl': ('libssl.a', 'libssl-dev'),
    'crypto': ('libcrypto.a', 'libssl-dev'),
    ':libjitterentropy.a': ('libjitterentropy.a', 'libjitterentropy3-dev'),
    'z': ('libz.a', 'zlib1g-dev'),
    'zstd': ('libzstd.a', 'libzstd-dev'),
}
AUXILIARIES = {entry[0] for key, entry in ARCHIVES.items() if key not in ('ssl', 'crypto')}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*arguments):
    return subprocess.check_output(arguments, text=True, timeout=30).strip()


def static_names(flags, library_directory):
    """Reject a changed distro recipe rather than silently widening this CI proof."""
    names = []
    for token in shlex.split(flags):
        if token in ('-pthread', '-ldl'):
            continue
        if token == '-L' + str(library_directory):
            continue
        if not token.startswith('-l') or token[2:] not in ARCHIVES:
            raise RuntimeError('Unrecognized static OpenSSL dependency: ' + token)
        if token[2:] not in names:
            names.append(token[2:])
    if not {'ssl', 'crypto'}.issubset(names):
        raise RuntimeError('Static OpenSSL metadata omitted ssl/crypto')
    return names


def package_file(path, expected_package):
    if not path.is_file() or path.is_symlink():
        raise RuntimeError('Missing regular static dependency: ' + str(path))
    rows = command('dpkg-query', '-S', str(path)).splitlines()
    if len(rows) != 1 or ': ' not in rows[0]:
        raise RuntimeError('Ambiguous package ownership: ' + str(path))
    owner, owned_path = rows[0].split(': ', 1)
    if owner.split(':')[0] != expected_package or owned_path != str(path):
        raise RuntimeError('Unexpected static dependency package: ' + str(path))
    version = command('dpkg-query', '-W', '-f=${Version}', owner)
    return owner, version


def prepare(root, evidence_path):
    if root.exists() or evidence_path.exists():
        raise RuntimeError('Preserve existing static input evidence/prefix')
    multiarch = command('gcc', '-print-multiarch')
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', multiarch):
        raise RuntimeError('Invalid compiler multiarch')
    library_directory = Path('/usr/lib') / multiarch
    flags = command('pkg-config', '--libs', '--static', 'openssl')
    names = static_names(flags, library_directory)
    # Validate every input before creating the prefix, including distro transitive
    # dependencies absent from libssl-dev's Depends on affected Ubuntu releases.
    inputs = {}
    for name in names:
        basename, package = ARCHIVES[name]
        source = library_directory / basename
        owner, version = package_file(source, package)
        notice = Path('/usr/share/doc') / package / 'copyright'
        if not notice.is_file() or not notice.read_bytes().strip():
            raise RuntimeError('Missing dependency notice: ' + str(notice))
        inputs[basename] = {'path': str(source), 'sha256': sha(source),
                            'package': owner, 'packageVersion': version,
                            'licensePath': str(notice.resolve(strict=True)),
                            'licenseSha256': sha(notice)}
    shared = {}
    for basename in ('libssl.so.3', 'libcrypto.so.3'):
        source = (library_directory / basename).resolve(strict=True)
        owner, version = package_file(source, 'libssl3t64')
        if version != inputs['libcrypto.a']['packageVersion']:
            raise RuntimeError('OpenSSL static/shared package version mismatch')
        shared[basename] = {'path': str(source), 'sha256': sha(source),
                            'package': owner, 'packageVersion': version}
    if inputs['libssl.a']['packageVersion'] != inputs['libcrypto.a']['packageVersion']:
        raise RuntimeError('OpenSSL archive package versions differ')
    root.mkdir(parents=True)
    (root / 'lib/pkgconfig').mkdir(parents=True)
    (root / 'source-metadata').mkdir()
    shutil.copytree('/usr/include/openssl', root / 'include/openssl', symlinks=False)
    architecture_headers = Path('/usr/include') / multiarch / 'openssl'
    if architecture_headers.is_dir():
        shutil.copytree(architecture_headers, root / 'include/openssl', dirs_exist_ok=True)
    for basename, entry in inputs.items():
        destination = root / 'lib' / basename
        shutil.copyfile(entry['path'], destination)
        if sha(destination) != entry['sha256']:
            raise RuntimeError('Copied static input changed')
        entry['controlledPath'] = str(destination)
        notice = root / 'source-notices' / entry['package'].split(':')[0] / 'copyright'
        notice.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(entry['licensePath'], notice)
        if sha(notice) != entry['licenseSha256']:
            raise RuntimeError('Copied dependency notice changed')
        entry['sourceLicensePath'], entry['licensePath'] = entry['licensePath'], str(notice)
    metadata = {}
    for basename in ('openssl.pc', 'libssl.pc', 'libcrypto.pc'):
        source = library_directory / 'pkgconfig' / basename
        owner, version = package_file(source, 'libssl-dev')
        if version != inputs['libcrypto.a']['packageVersion']:
            raise RuntimeError('OpenSSL metadata/archive package versions differ')
        original = source.read_bytes()
        (root / 'source-metadata' / basename).write_bytes(original)
        text = original.decode()
        for key, value in (('prefix', str(root)), ('libdir', str(root / 'lib')),
                           ('includedir', str(root / 'include'))):
            text, count = re.subn(r'^' + key + r'=.*$', lambda _: key + '=' + value,
                                  text, flags=re.MULTILINE)
            if count != 1:
                raise RuntimeError('OpenSSL metadata missing unique ' + key)
        destination = root / 'lib/pkgconfig' / basename
        destination.write_text(text)
        metadata[basename] = {'package': owner, 'sha256': sha(source),
                              'preparedSha256': sha(destination)}
    if any(path.is_symlink() for path in root.rglob('*')):
        raise RuntimeError('Controlled prefix contains a symlink')
    evidence = {'package': 'libssl-dev', 'packageVersion': inputs['libcrypto.a']['packageVersion'],
                'multiarch': multiarch, 'pkgConfigStaticFlags': flags,
                'pkgConfigMetadata': metadata, 'sourceArchives': inputs,
                'auxiliaryArchives': sorted(set(inputs) & AUXILIARIES),
                'cohabitationLibraries': shared, 'qualificationClaimed': False}
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + '\n')
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.absolute()
    evidence = prepare(root, args.evidence.absolute())
    with open(os.environ['GITHUB_ENV'], 'a') as output:
        output.write('ODBCPP_STATIC_OPENSSL_ROOT=' + str(root) + '\n')
        output.write('PKG_CONFIG_LIBDIR=' + str(root / 'lib/pkgconfig') + '\n')
        output.write('PKG_CONFIG_PATH=\n')
        for role in ('SSL', 'CRYPTO'):
            output.write('ODBCPP_COHABITATION_' + role + '_LIBRARY=' +
                         evidence['cohabitationLibraries']['lib' + role.lower() + '.so.3']['path'] + '\n')


if __name__ == '__main__':
    main()

"""Internal Linux static-driver archive proof; not a public installer or SDK package."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('archive_checks', REPO / 'tests/security/awslc/test_relocated_package.py')
archive_checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive_checks)
ROOT_NAME = 'odbcpp-openssl-static-proof'
LICENSES = {'licenses/openssl/copyright', 'licenses/spdlog/LICENSE', 'licenses/fmt/LICENSE'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage(build, destination, openssl_license):
    manifest_path = build / 'odbcpp-crypto-manifest.json'
    artifact_path = build / 'odbcpp-crypto-artifact-evidence.json'
    manifest = json.loads(manifest_path.read_text())
    artifact = json.loads(artifact_path.read_text())
    source_path = build.parent / 'openssl-static-source-evidence.json'
    source = json.loads(source_path.read_text())
    driver = build / 'libodbcpp.so'
    if (manifest['provider'] != 'OPENSSL' or manifest['requestedLinkage'] != 'BUNDLED_STATIC'
            or manifest['fipsClaimed'] is not False or artifact['platform'] != 'Linux'
            or artifact['observedDependencyForm'] != 'static'
            or artifact['provider'] != 'OPENSSL' or artifact['requestedLinkage'] != 'BUNDLED_STATIC'
            or artifact['qualificationClaimed'] is not False
            or artifact['dependencyFormVerified'] is not True
            or artifact['staticLinkMapVerified'] is not True
            or artifact['staticArchiveMemberEvidenceVerified'] is not True
            or artifact['sslLinkInputSha256'] != source['sourceArchives']['libssl.a']['sha256']
            or artifact['cryptoLinkInputSha256'] != source['sourceArchives']['libcrypto.a']['sha256']
            or artifact['artifactSha256'] != sha(driver)):
        raise RuntimeError('Static package inputs do not match inspected Linux driver')
    spdlog = build / '_deps/spdlog-src'
    fmt_header = (spdlog / 'include/spdlog/fmt/bundled/format.h').read_text()
    license_end = fmt_header.find('*/')
    version = re.search(r'^#define FMT_VERSION (\d+)$',
                       (spdlog / 'include/spdlog/fmt/bundled/base.h').read_text(), re.MULTILINE)
    if not fmt_header.startswith('/*') or license_end < 0 or version is None:
        raise RuntimeError('Pinned fmt license/version not found')
    sources = {
        'licenses/openssl/copyright': openssl_license.read_bytes(),
        'licenses/spdlog/LICENSE': (spdlog / 'LICENSE').read_bytes(),
        'licenses/fmt/LICENSE': (fmt_header[:license_end + 2] + '\n').encode(),
    }
    if any(not content.strip() for content in sources.values()):
        raise RuntimeError('Dependency license is empty')
    (destination / 'lib').mkdir(parents=True)
    shutil.copyfile(driver, destination / 'lib/libodbcpp.so')
    (destination / 'lib/libodbcpp.so').chmod(0o755)
    for name, content in sources.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    package_manifest = {
        'schemaVersion': 1, 'provider': 'OPENSSL', 'linkage': 'BUNDLED_STATIC',
        'compileVersion': manifest['compileVersion'], 'fipsClaimed': False,
        'qualificationClaimed': False, 'sourceProvenanceVerified': False,
        'driverSha256': sha(driver), 'configureManifestSha256': sha(manifest_path),
        'artifactEvidenceSha256': sha(artifact_path),
        'sourceEvidenceSha256': sha(source_path),
        'declaredOpenSslRecipe': {'package': source['package'], 'version': source['packageVersion'],
            'archiveHashes': {name: entry['sha256'] for name, entry in source['sourceArchives'].items()}},
        'licenseFiles': {name: sha(destination / name) for name in sorted(LICENSES)},
        'loggingRecipeSha256': sha(REPO / 'cmake/FindLogging.cmake'),
        'fmtVersionNumber': int(version[1]),
        'scope': 'Internal driver-only runtime archive; distro source recipe and licenses, not full source attestation',
    }
    (destination / 'package-manifest.json').write_text(json.dumps(package_manifest, indent=2) + '\n')
    return validate(destination)


def validate(package):
    manifest = json.loads((package / 'package-manifest.json').read_text())
    if (manifest['schemaVersion'] != 1 or manifest['provider'] != 'OPENSSL' or manifest['linkage'] != 'BUNDLED_STATIC'
            or manifest['fipsClaimed'] is not False or manifest['qualificationClaimed'] is not False
            or manifest['sourceProvenanceVerified'] is not False):
        raise RuntimeError('Unsupported static package claim')
    expected = LICENSES | {'lib/libodbcpp.so', 'package-manifest.json'}
    actual = archive_checks.inventory(package)
    if set(actual) != expected or set(manifest['licenseFiles']) != LICENSES:
        raise RuntimeError('Driver-only file/license inventory mismatch')
    if manifest['driverSha256'] != sha(package / 'lib/libodbcpp.so'):
        raise RuntimeError('Packaged driver hash mismatch')
    for name, digest in manifest['licenseFiles'].items():
        path = package / name
        if not path.read_bytes().strip() or sha(path) != digest:
            raise RuntimeError('Packaged license missing, empty or hash mismatch')
    return manifest


def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'openssl-license', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    build, output = Path(args.build).resolve(strict=True), Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.pop('LD_PRELOAD', None)
    env.pop('LD_LIBRARY_PATH', None)
    tls = Path(env['ODBCPP_POSTGRES_TLS_DIR']).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='odbcpp-static-stage-') as temporary:
        staging = Path(temporary) / ROOT_NAME
        manifest = stage(build, staging, Path(args.openssl_license).resolve(strict=True))
        expected = archive_checks.inventory(staging)
        archive = output / 'driver-only.tar.gz'
        with tarfile.open(archive, 'w:gz') as stream:
            stream.add(staging, arcname=ROOT_NAME)
        shutil.rmtree(staging)
        package = archive_checks.extract_verified(archive, output / 'extracted', expected, ROOT_NAME)
    validate(package)
    driver = (package / 'lib/libodbcpp.so').resolve(strict=True)
    # A fresh ctypes host uses the runtime archive, with no provider preload.
    code = '''import ctypes,json,pathlib,sys
ctypes.CDLL(sys.argv[1])
lines=pathlib.Path('/proc/self/maps').read_text().splitlines()
providers=[line for line in lines if '/libssl' in line or '/libcrypto' in line]
if providers: raise RuntimeError('Static driver mapped a shared crypto provider: '+repr(providers))
if not any(sys.argv[1] in line for line in lines): raise RuntimeError('Extracted driver was not mapped')
print('Extracted static driver load and no shared crypto mappings: PASS')
'''
    subprocess.run([sys.executable, '-c', code, str(driver)], check=True, env=env, timeout=20)
    base = (f'DRIVER={{{driver}}};SERVER=127.0.0.1;PORT={env.get("PGPORT", "5432")};'
            'DATABASE=postgres;UID=postgres;PWD=postgres;SSL=1;')
    env.update({
        'ODBCPP_CRYPTO_PROFILE_DRIVER_PATH': str(driver),
        'ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION': base + f'SSLCAFILE={tls}/server.crt;',
        'ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION': base + f'SSLCAFILE={tls}/wrong.crt;',
        'ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION': base.replace('SERVER=127.0.0.1',
            'SERVER=' + env.get('ODBCPP_CRYPTO_PROFILE_WRONG_HOST', 'wrong-host.invalid')) + f'SSLCAFILE={tls}/server.crt;',
    })
    report = output / 'extracted-driver-live.xml'
    subprocess.run([str(build / 'tests/it_crypto_profile_live'), '--gtest_output=xml:' + str(report)],
                   check=True, env=env, timeout=60)
    root = ET.parse(report).getroot()
    cases = list(root.iter('testcase'))
    if (len(cases) != 3 or {case.get('name') for case in cases} != {
            'CompletesVerifiedTlsScramQuery', 'RejectsUntrustedCertificate', 'RejectsHostnameMismatch'}
            or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped'))):
        raise RuntimeError('Extracted static driver live cases missing or unsuccessful')
    evidence = {
        'schemaVersion': 1, 'profile': 'OPENSSL/BUNDLED_STATIC/Linux',
        'qualificationClaimed': False, 'runtimeOnlyArchiveVerified': True,
        'sharedCryptoMapped': False, 'verifiedTlsScramQuery': True,
        'trustAndHostnameRejectionsVerified': True,
        'archiveSha256': sha(archive), 'packageManifestSha256': sha(package / 'package-manifest.json'),
        'driverSha256': manifest['driverSha256'], 'licenseFiles': manifest['licenseFiles'],
        'liveReportSha256': sha(report), 'inventory': expected,
    }
    (output / 'package-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()

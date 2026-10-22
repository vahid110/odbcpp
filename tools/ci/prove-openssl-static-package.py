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
AUXILIARY_LICENSES = {'libjitterentropy.a': 'licenses/jitterentropy/copyright',
                      'libz.a': 'licenses/zlib/copyright',
                      'libzstd.a': 'licenses/zstd/copyright'}


def auxiliary_licenses(names):
    if len(names) != len(set(names)) or not set(names).issubset(AUXILIARY_LICENSES):
        raise RuntimeError('Unknown or duplicate static auxiliary dependency')
    return {AUXILIARY_LICENSES[name] for name in names}


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
    auxiliaries = source.get('auxiliaryArchives', [])
    auxiliary_licenses(auxiliaries)
    if set(source['sourceArchives']) != {'libssl.a', 'libcrypto.a'} | set(auxiliaries):
        raise RuntimeError('Static source archive/license inventory mismatch')
    configured_aux = {Path(entry['path']).name: entry
                      for entry in manifest.get('auxiliaryStaticLinkInputs', [])}
    inspected_aux = {Path(entry['path']).name: entry
                     for entry in artifact.get('auxiliaryStaticLinkInputs', [])}
    if (len(configured_aux) != len(manifest.get('auxiliaryStaticLinkInputs', [])) or
            len(inspected_aux) != len(artifact.get('auxiliaryStaticLinkInputs', [])) or
            set(configured_aux) != set(auxiliaries) or set(inspected_aux) != set(auxiliaries)):
        raise RuntimeError('Static auxiliary dependency inventory mismatch')
    for name in auxiliaries:
        entry, configured, inspected = source['sourceArchives'][name], configured_aux[name], inspected_aux[name]
        if (configured['sha256'] != entry['sha256'] or inspected['sha256'] != entry['sha256'] or
                inspected['path'] != configured['path'] or
                configured['path'] != entry['controlledPath'] or
                inspected['linkMapLoadVerified'] is not True or
                type(inspected['archiveMembersObserved']) is not bool):
            raise RuntimeError('Static auxiliary dependency binding mismatch')
        notice = Path(entry['licensePath'])
        if sha(notice) != entry['licenseSha256']:
            raise RuntimeError('Static auxiliary dependency license changed')
        sources[AUXILIARY_LICENSES[name]] = notice.read_bytes()
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
            'archiveHashes': {name: source['sourceArchives'][name]['sha256']
                              for name in ('libssl.a', 'libcrypto.a')}},
        'declaredAuxiliaryRecipes': {name: {
            'package': source['sourceArchives'][name]['package'],
            'version': source['sourceArchives'][name]['packageVersion'],
            'archiveSha256': source['sourceArchives'][name]['sha256'],
            'licenseSha256': source['sourceArchives'][name]['licenseSha256']}
            for name in auxiliaries},
        'auxiliaryArchives': auxiliaries,
        'licenseFiles': {name: sha(destination / name) for name in sorted(sources)},
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
    licenses = LICENSES | auxiliary_licenses(manifest.get('auxiliaryArchives', []))
    if set(manifest.get('declaredAuxiliaryRecipes', {})) != set(manifest.get('auxiliaryArchives', [])):
        raise RuntimeError('Static auxiliary recipe inventory mismatch')
    expected = licenses | {'lib/libodbcpp.so', 'package-manifest.json'}
    actual = archive_checks.inventory(package)
    if set(actual) != {str(Path(name)) for name in expected} or set(manifest['licenseFiles']) != licenses:
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
    # Python itself may already map zlib. Bind actual before/after identities;
    # the preceding artifact/map/undefined-symbol gates still forbid borrowing
    # a preexisting shared dependency instead of the controlled static inputs.
    load_report = output / 'static-load-observation.json'
    subprocess.run([sys.executable, str(Path(__file__).with_name('static_load_probe.py')),
                    '--driver', str(driver), '--sha256', manifest['driverSha256'],
                    '--output', str(load_report)], check=True, env=env, timeout=20)
    load = json.loads(load_report.read_text())
    if (load['validationPassed'] is not True or load['qualificationClaimed'] is not False
            or load['driver'] != str(driver) or load['driverPin']['sha256'] != manifest['driverSha256']
            or load['sharedCryptoMapped'] is not False or load['noNewSharedAuxiliaryMappings'] is not True):
        raise RuntimeError('Static driver load observation is not bound to this archive')
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
        'sharedCryptoMapped': False, 'noNewSharedAuxiliaryMappings': True,
        'baselineAuxiliaryMappings': load['baselineAuxiliaryMappings'],
        'afterAuxiliaryMappings': load['afterAuxiliaryMappings'],
        'newSharedAuxiliaryMappings': load['newSharedAuxiliaryMappings'],
        'loadObservationSha256': sha(load_report), 'verifiedTlsScramQuery': True,
        'trustAndHostnameRejectionsVerified': True,
        'archiveSha256': sha(archive), 'packageManifestSha256': sha(package / 'package-manifest.json'),
        'driverSha256': manifest['driverSha256'], 'licenseFiles': manifest['licenseFiles'],
        'liveReportSha256': sha(report), 'inventory': expected,
    }
    (output / 'package-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()

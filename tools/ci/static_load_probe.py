"""Observe a verified static driver's load in a fresh Linux Python host."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def kind(path):
    name = Path(path.removesuffix(' (deleted)')).name
    if re.match(r'^lib(?:ssl|crypto)\.so(?:\.|$)', name):
        return 'crypto'
    if re.match(r'^lib(?:jitterentropy|zstd|z)\.so(?:\.|$)', name):
        return 'auxiliary'
    return None


def mappings(text):
    """Keep file identity and each mapping range; a basename is not custody."""
    rows = []
    for line in text.splitlines():
        fields = line.split(None, 5)
        if len(fields) < 5:
            raise RuntimeError('Malformed process mapping')
        address, permissions, offset, device, inode = fields[:5]
        if (not re.fullmatch(r'[0-9a-f]+-[0-9a-f]+', address)
                or not re.fullmatch(r'[r-][w-][x-][ps]', permissions)
                or not re.fullmatch(r'[0-9a-f]+', offset)
                or not re.fullmatch(r'[0-9a-f]+:[0-9a-f]+', device)
                or not inode.isdecimal()):
            raise RuntimeError('Malformed process mapping fields')
        start, end = (int(value, 16) for value in address.split('-'))
        if start >= end:
            raise RuntimeError('Invalid process mapping range')
        if len(fields) == 5 or not fields[5].startswith('/'):
            continue  # Anonymous mappings have no dependency-file identity.
        path = re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), fields[5])
        major, minor = (int(value, 16) for value in device.split(':'))
        row = dict(path=path, major=major, minor=minor, inode=int(inode),
                   start=start, end=end, offset=int(offset, 16), permissions=permissions)
        if row in rows:
            raise RuntimeError('Duplicate process mapping')
        rows.append(row)
    if not rows:
        raise RuntimeError('Process mapping snapshot has no file mappings')
    return rows


def identity(row):
    return row['major'], row['minor'], row['inode']


def validate(before, after, driver_path, driver_identity):
    crypto = [row for row in before + after if kind(row['path']) == 'crypto']
    if crypto:
        raise RuntimeError('Static clean-host proof mapped shared crypto: ' + repr(crypto))
    before_aux = [row for row in before if kind(row['path']) == 'auxiliary']
    after_aux = [row for row in after if kind(row['path']) == 'auxiliary']
    if any(row['path'].endswith(' (deleted)') for row in before_aux + after_aux):
        raise RuntimeError('Auxiliary mapping refers to a deleted file')
    key = lambda row: json.dumps(row, sort_keys=True)
    if sorted(before_aux, key=key) != sorted(after_aux, key=key):
        raise RuntimeError('Driver load introduced or changed auxiliary mappings: ' +
                           repr(dict(before=before_aux, after=after_aux)))
    before_driver = [row for row in before
                     if identity(row) == driver_identity or row['path'] == driver_path]
    if before_driver:
        raise RuntimeError('Extracted driver was already mapped before load')
    after_driver = [row for row in after
                    if identity(row) == driver_identity or row['path'] == driver_path]
    if (not after_driver or not any('x' in row['permissions'] for row in after_driver)
            or any(identity(row) != driver_identity or row['path'] != driver_path
                   for row in after_driver)):
        raise RuntimeError('Loaded driver mapping does not match the extracted file identity')
    return dict(sharedCryptoMapped=False, baselineAuxiliaryMappings=before_aux,
                afterAuxiliaryMappings=after_aux, newSharedAuxiliaryMappings=[],
                noNewSharedAuxiliaryMappings=True, driverMappings=after_driver)


def file_pin(path):
    info = path.stat()
    # hashlib may load the host's shared libcrypto. Hash in a separate fixed
    # coreutils process so the observer itself remains a clean crypto host.
    digest = subprocess.run(['/usr/bin/sha256sum', '--zero', '--', str(path)],
                            check=True, capture_output=True, timeout=5).stdout
    if (not re.fullmatch(b'[0-9a-f]{64}', digest[:64]) or digest[64:66] != b'  '
            or digest[66:] != os.fsencode(path) + b'\0'):
        raise RuntimeError('Malformed extracted driver hash output')
    return dict(major=os.major(info.st_dev), minor=os.minor(info.st_dev), inode=info.st_ino,
                bytes=info.st_size, sha256=digest[:64].decode('ascii'))


def snapshot():
    path = Path('/proc/self/maps')
    with path.open() as stream:
        text = stream.read(8 * 1024 * 1024 + 1)
    if len(text) > 8 * 1024 * 1024:
        raise RuntimeError('Process mapping snapshot exceeded its bound')
    return mappings(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'linux' or not re.fullmatch('[0-9a-f]{64}', args.sha256):
        raise RuntimeError('Require Linux and an exact expected driver hash')
    driver = args.driver.resolve(strict=True)
    if not driver.is_file() or args.output.exists():
        raise RuntimeError('Require a regular extracted driver and a new observation file')
    pin = file_pin(driver)
    if pin['sha256'] != args.sha256:
        raise RuntimeError('Extracted driver hash changed before load')
    record = dict(schemaVersion=1, driver=str(driver), driverPin=pin,
                  validationPassed=False, qualificationClaimed=False)
    try:
        # All imports are complete before measuring the interpreter's baseline.
        before = snapshot()
        record['beforeMappings'] = before
        library = ctypes.CDLL(str(driver), mode=os.RTLD_NOW | os.RTLD_LOCAL)
        after = snapshot()
        record['afterMappings'] = after
        if file_pin(driver) != pin:
            raise RuntimeError('Extracted driver identity/bytes changed during load')
        record.update(validate(before, after, str(driver),
                               (pin['major'], pin['minor'], pin['inode'])))
        record['validationPassed'] = True
        # Retain the handle until after observation and evidence publication.
        assert library._handle
    finally:
        with args.output.open('x') as stream:
            json.dump(record, stream, indent=2, sort_keys=True)
            stream.write('\n')
    print('Extracted static driver load, no shared crypto and no new auxiliary mappings: PASS')


if __name__ == '__main__':
    main()

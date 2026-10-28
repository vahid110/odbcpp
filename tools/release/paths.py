"""Release-tool filesystem boundaries, not a sandbox for the invoking operator.

CLI roots may intentionally be outside cwd (CMake out-of-tree builds, artifact
intake, exported source). Treat filenames inside metadata as untrusted members
of those selected roots. The operator must own roots against concurrent writers;
these helpers do not promise race-free access in an attacker-writable directory.
"""
import os
from pathlib import Path
import re
import tempfile


def selected_root(path):
    """Canonicalize the operator-selected directory, including intended aliases."""
    root = Path(path).resolve(strict=True)
    if not root.is_dir():
        raise ValueError('Selected root is not a directory')
    return root


def member(root, name):
    """Constrain a portable relative member, including its symlink ancestors."""
    if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.+ /-]+', name)
            or any(part in ('', '.', '..') for part in name.split('/'))
            or any(part.endswith((' ', '.')) for part in name.split('/'))
            or any(re.fullmatch(r'(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])',
                                part.split('.')[0], re.I) for part in name.split('/'))):
        raise ValueError('Unsafe release member name')
    root = selected_root(root)
    path = root.joinpath(*name.split('/'))
    if not path.resolve().is_relative_to(root):
        raise ValueError('Release member escapes selected root')
    return path


def regular(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'Expected regular file: {path}')
    return path


def selected_output(path):
    """The operator selects this destination, metadata never selects its parent."""
    path = Path(path)
    return member(selected_root(path.parent), path.name)


def fresh_directory(path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path = selected_output(path)
    if path.is_symlink():
        raise ValueError('Output directory may not be a symlink')
    path.mkdir(exist_ok=False)
    return path


def write_generated(path, text):
    """Replace our generated file atomically, without writing through links."""
    path = selected_output(path)
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError('Generated output must be a regular file')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix='.odbcpp-version-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()

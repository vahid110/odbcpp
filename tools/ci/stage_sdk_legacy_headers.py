"""Prepare the exact legacy install declaration tree from canonical owners."""
import argparse
import json
from pathlib import Path
from sdk_legacy_header_projection import MEMBERS, OWNERS, MAX_HEADER, MAX_TOTAL, project, restage

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--owners-manifest', required=True)
    args = parser.parse_args()
    root = Path(args.source_root).resolve(strict=True)
    manifest = Path(args.manifest).read_text(encoding='utf-8').splitlines()
    if manifest != list(MEMBERS):
        raise ValueError('Legacy installed membership must remain exact')
    owners = Path(args.owners_manifest).read_text(encoding='utf-8').splitlines()
    if owners != sorted(OWNERS.values()):
        raise ValueError('Legacy owner dependency membership must remain exact')
    sources = {}
    resolved = set()
    total = 0
    for owner in OWNERS.values():
        path = (root / owner).resolve(strict=True)
        path.relative_to(root)
        if not path.is_file() or path in resolved:
            raise ValueError('Missing or aliased canonical source owner')
        resolved.add(path)
        with path.open("rb") as stream:
            value = stream.read(MAX_HEADER + 1)
        total += len(value)
        if len(value) > MAX_HEADER or total > MAX_TOTAL:
            raise ValueError("Canonical source byte bound exceeded")
        sources[owner] = value
    outcome = restage(Path(args.output), project(sources))
    print(json.dumps({'published': outcome.published,
                      'cleanup_complete': outcome.cleanup_complete,
                      'cleanup_uncertain': outcome.cleanup_uncertain,
                      'members': len(MEMBERS)}))
    if not outcome.published or not outcome.cleanup_complete or outcome.cleanup_uncertain:
        return 2
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Download complete native audits, then verify integrity without executing a simulation.
Usage: research_audit.py download API_BASE RUN_ID NEW_DIRECTORY
       research_audit.py verify DIRECTORY
Completed runs: exsim_research verify DIRECTORY/runner (current-build reproduction).
Failed/cancelled: integrity verification only; cancellation timing is not replayable.
"""
import hashlib
import json
from pathlib import Path
import sys
import urllib.request


def verify(root):
    manifest = json.loads((root / 'audit-inventory.json').read_text())
    for item in manifest['items']:
        name = Path(item['name'])
        if name.is_absolute() or '..' in name.parts:
            raise ValueError('unsafe inventory name')
        path = root / name
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('artifact escapes package directory')
        if not item['present']:
            if path.exists() or path.is_symlink():
                raise ValueError(f'unexpected artifact: {name}')
            continue
        if path.is_symlink() or not path.is_file() or path.stat().st_size != int(item['sizeBytes']):
            raise ValueError(f'missing or wrong size: {name}')
        with path.open('rb') as data:
            hasher = hashlib.sha256()
            while True:
                block = data.read(65536)
                if not block:
                    break
                hasher.update(block)
            digest = hasher.hexdigest()
        if digest != item['sha256']:
            raise ValueError(f'integrity mismatch: {name}')
    print('Integrity verified; reproduction supported:', manifest['reproductionSupported'])


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'verify':
        verify(Path(sys.argv[2])); return
    if len(sys.argv) != 5 or sys.argv[1] != 'download':
        raise SystemExit(__doc__)
    _, _, base, run, directory = sys.argv
    if not run.startswith('run_') or not run[4:].isalnum():
        raise ValueError('invalid run ID')
    root = Path(directory)
    root.mkdir(exist_ok=False)
    endpoint = base.rstrip('/') + '/v1/runs/' + run + '/audit-export'
    def fetch(query=''):
        with urllib.request.urlopen(endpoint + query, timeout=120) as response:
            return json.load(response)
    manifest = fetch()
    (root / 'audit-inventory.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for item in manifest['items']:
        if not item['present']:
            continue
        path = root / item['name']
        if path.is_absolute() and not path.is_relative_to(root):
            raise ValueError('unsafe artifact')
        if '..' in Path(item['name']).parts:
            raise ValueError('unsafe artifact')
        path.parent.mkdir(parents=True, exist_ok=True)
        offset = 0
        with path.open('xb') as output:
            while True:
                page = fetch('?artifactIndex=' + item['artifactIndex'] + '&offset=' + str(offset))
                content = bytes.fromhex(page['contentHex'])
                output.write(content)
                if page['nextOffset'] is None:
                    break
                next_offset = int(page['nextOffset'])
                if next_offset <= offset or next_offset > int(item['sizeBytes']):
                    raise ValueError('invalid transfer continuation')
                offset = next_offset
    verify(root)

if __name__ == '__main__':
    main()

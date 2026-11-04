"""Record what every launcher-compiled object was built from, so a later run can predict reuse.

A restored compiler cache can only serve an object whose inputs did not change since the
run that saved it. A raw hit rate cannot tell "the cache is broken" from "a header almost
every translation unit includes changed" (#1678 edited Debug/Instrumentor.h and read as
4.99% near-zero-warm on a correct cache). This manifest closes that gap: the producer saves
one input hash per object beside its cache, and the consumer counts the objects whose hash
is unchanged -- exactly the ones the cache must serve.

An object's hash covers its full compile command, its source file and every header ninja
recorded for it (`ninja -t deps`, from /showIncludes), by content. Objects compiled without
the cache launcher, with an MSVC PCH (/Yu, whose /showIncludes omits the PCH's headers) or
without a VALID dependency record are listed as unpredictable and never counted as reusable.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SCHEMA = 1
RESPONSE_FILE = re.compile(r'(^|\s)"?@\S')


def normalise(path):
    return os.path.normcase(os.path.normpath(path)).replace('\\', '/')


def parse_deps(text):
    """`ninja -t deps` output -> {output: [dependency, ...]} for VALID records only."""
    records, current = {}, None
    for line in text.splitlines():
        if not line.strip():
            current = None
        elif line[0] not in ' \t':
            output, _, status = line.partition(': #deps ')
            current = output if status.rstrip().endswith('(VALID)') else None
            if current is not None:
                records[current] = []
        elif current is not None:
            records[current].append(line.strip())
    return records


def unpredictable_reason(entry, launcher, deps):
    command = entry.get('command', '')
    if launcher not in command.lower():
        return None  # not a cache request at all; neither predicted nor counted
    if RESPONSE_FILE.search(command):
        # main() asks ninja to expand these (`compdb -x`). One left over hashes a path, not
        # the flags in it, and hides a /Yu from the check below.
        return 'compile command reads a response file'
    if '/yu' in command.lower() or '-yu' in command.lower():
        return 'uses an MSVC precompiled header'
    if entry['output'] not in deps:
        return 'no VALID ninja dependency record'
    return ''


def build_manifest(compdb, deps, build_dir, read_file, launcher='sccache', identity=None):
    """Pure core: compdb entries + parsed deps -> manifest. read_file(path) -> bytes or None."""
    file_hashes = {}

    def file_hash(path, base=build_dir):
        key = normalise(path if os.path.isabs(path) else os.path.join(base, path))
        if key not in file_hashes:
            content = read_file(key)
            file_hashes[key] = hashlib.sha256(content).hexdigest() if content is not None else None
        return key, file_hashes[key]

    objects, unpredictable = {}, {}
    for entry in compdb:
        output = entry.get('output', '')
        if not output.endswith(('.obj', '.o')):
            continue
        reason = unpredictable_reason(entry, launcher, deps)
        if reason is None:
            continue
        if reason:
            unpredictable[output] = reason
            continue
        digest = hashlib.sha256()
        digest.update(entry['command'].encode('utf-8') + b'\0')
        missing = None
        inputs = [(entry['file'], entry.get('directory') or build_dir)]
        inputs += [(path, build_dir) for path in sorted(set(deps[output]))]
        for path, base in inputs:
            key, value = file_hash(path, base)
            if value is None:
                missing = key
                break
            digest.update(f'{key}\0{value}\0'.encode('utf-8'))
        if missing:
            unpredictable[output] = f'input {missing} is unreadable'
        else:
            objects[output] = digest.hexdigest()
    return {'schema': SCHEMA, 'identity': identity or {}, 'objects': objects,
            'unpredictable': unpredictable,
            'files': {k: v for k, v in sorted(file_hashes.items()) if v is not None}}


def compare(restored, current):
    """Objects the restored cache must serve: same input hash in both manifests."""
    if not isinstance(restored, dict) or restored.get('schema') != SCHEMA:
        raise ValueError('restored manifest has an unknown schema')
    if not isinstance(current, dict) or current.get('schema') != SCHEMA:
        raise ValueError('current manifest has an unknown schema')
    before, after = restored['objects'], current['objects']
    reusable = sum(1 for output, value in after.items() if before.get(output) == value)
    old_files, new_files = restored.get('files', {}), current.get('files', {})
    changed_files = sorted(p for p, v in new_files.items() if p in old_files and old_files[p] != v)
    return {'predicted_reusable': reusable,
            'predicted_changed': len(after) - reusable,
            'unpredictable': len(current.get('unpredictable', {})),
            'changed_files': len(changed_files),
            'changed_file_sample': changed_files[:10],
            'producer': restored.get('identity', {})}


def read_bytes(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return None


def make_program(build_dir):
    """The ninja CMake configured the tree with, so the deps log is read by its own writer."""
    cache = Path(build_dir) / 'CMakeCache.txt'
    if cache.is_file():
        for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('CMAKE_MAKE_PROGRAM:'):
                return line.partition('=')[2].strip() or 'ninja'
    return 'ninja'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--config', default='Release', help="Ninja Multi-Config name; '' for build.ninja")
    parser.add_argument('--output', required=True)
    parser.add_argument('--run-id', default=os.environ.get('GITHUB_RUN_ID', ''))
    parser.add_argument('--sha', default=os.environ.get('GITHUB_SHA', ''))
    args = parser.parse_args()
    build_dir = os.path.abspath(args.build_dir)
    ninja = [make_program(build_dir), '-C', build_dir]
    if args.config:
        ninja += ['-f', f'build-{args.config}.ninja']
    # -x: hash the flags a response file holds, not its path.
    compdb = json.loads(subprocess.run(ninja + ['-t', 'compdb', '-x'], check=True, capture_output=True,
                                       text=True).stdout)
    deps = parse_deps(subprocess.run(ninja + ['-t', 'deps'], check=True, capture_output=True,
                                     text=True, errors='replace').stdout)
    manifest = build_manifest(compdb, deps, build_dir, read_bytes,
                              identity={'run_id': args.run_id, 'sha': args.sha})
    Path(args.output).write_text(json.dumps(manifest, separators=(',', ':')) + '\n', encoding='utf-8')
    print(f"object manifest: {len(manifest['objects'])} predictable objects, "
          f"{len(manifest['unpredictable'])} unpredictable, {len(manifest['files'])} input files")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

"""Rebuild an approved dog from editable source assets into a new directory.

Example: python tools/dog-authoring/rebuild.py --breed Samoyed --output C:/temp/Samoyed-build
The native cook uses the Release OloEngine-Tests executable, or --test-exe.
Use --mesh-only while editing; it deliberately leaves native cooking incomplete.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'OloEditor/SandboxProject/Assets'
SOURCES = Path(__file__).resolve().parent / 'sources'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def compare_mesh(shipped, rebuilt):
    """Pin topology, skin, animation and layout exactly; bound morph roundoff."""
    a, b = json.loads(shipped.read_text()), json.loads(rebuilt.read_text())
    maximum = 0.0
    # NumPy/BLAS implementations can differ in the last bit of a computed delta.
    # Only morph position/normal arrays may differ, below one nanometre for
    # positions. All remaining bytes (including base geometry and animation)
    # must be exact. This is not a tolerance on the whole model.
    targets = {index for mesh in a['meshes'] for primitive in mesh['primitives']
               for target in primitive.get('targets', []) for semantic, index in target.items()
               if semantic in ('POSITION', 'NORMAL')}
    referenced = set(targets)
    for mesh in a['meshes']:
        for primitive in mesh['primitives']:
            referenced.update(primitive['attributes'].values())
            referenced.add(primitive['indices'])
    for animation in a.get('animations', []):
        for sampler in animation['samplers']:
            referenced.update((sampler['input'], sampler['output']))
    referenced.update(skin['inverseBindMatrices'] for skin in a.get('skins', []))
    # Refinement appends corrected arrays; superseded morph deltas remain in the
    # editable buffer but are no longer referenced by the final mesh.
    targets.update(index for index, accessor in enumerate(a['accessors'])
                   if index not in referenced and accessor['componentType'] == 5126 and accessor['type'] == 'VEC3')
    if len(a['accessors']) != len(b['accessors']):
        raise ValueError(f'Rebuilt accessor count differs: {shipped.name}')
    # The writers derive morph bounds from the same float arrays. Permit only
    # the same absolute roundoff there, then compare all other metadata exactly.
    for index in sorted(targets):
        left, right = a['accessors'][index], b['accessors'][index]
        for bound in ('min', 'max'):
            if (bound in left) != (bound in right):
                raise ValueError(f'Morph accessor {index} {bound} presence differs: {shipped.name}')
            if bound not in left:
                continue
            for values in (left[bound], right[bound]):
                if not isinstance(values, list) or len(values) != 3 or any(type(v) not in (int, float) for v in values):
                    raise ValueError(f'Morph accessor {index} has invalid {bound}: {shipped.name}')
                if not np.isfinite(values).all():
                    raise ValueError(f'Morph accessor {index} has non-finite {bound}: {shipped.name}')
            difference = float(np.max(np.abs(np.asarray(left[bound], dtype=float) - np.asarray(right[bound], dtype=float))))
            if difference > 1e-9:
                raise ValueError(f'Morph accessor {index} {bound} differs by {difference}: {shipped.name}')
            maximum = max(maximum, difference)
            right[bound] = left[bound]
    if a != b:
        raise ValueError(f'Rebuilt glTF structure differs: {shipped.name}')
    old = (shipped.parent / a['buffers'][0]['uri']).read_bytes()
    new = bytearray((rebuilt.parent / b['buffers'][0]['uri']).read_bytes())
    for index in sorted(targets):
        accessor = a['accessors'][index]
        view = a['bufferViews'][accessor['bufferView']]
        assert accessor['componentType'] == 5126 and accessor['type'] == 'VEC3'
        assert view.get('byteStride', 12) == 12
        offset = view.get('byteOffset', 0) + accessor.get('byteOffset', 0)
        size = accessor['count'] * 12
        x = np.frombuffer(old, dtype='<f4', count=accessor['count'] * 3, offset=offset)
        y = np.frombuffer(new, dtype='<f4', count=accessor['count'] * 3, offset=offset)
        difference = float(np.max(np.abs(x.astype(float) - y.astype(float)), initial=0.0))
        if not np.isfinite(difference) or difference > 1e-9:
            raise ValueError(f'Morph accessor {index} differs by {difference}: {shipped.name}')
        maximum = max(maximum, difference)
        del y
        new[offset:offset + size] = old[offset:offset + size]
    if new != old:
        raise ValueError(f'Non-morph buffer data differs: {shipped.name}')
    return maximum


def same_png_texels(shipped, rebuilt):
    """Allow compression changes only for the authored single-frame 8-bit PNGs."""
    with shipped.open('rb') as left, rebuilt.open('rb') as right:
        left_header, right_header = left.read(26), right.read(26)
    if len(left_header) != 26 or len(right_header) != 26 or left_header[24] != 8 or left_header[24:26] != right_header[24:26]:
        return False
    with Image.open(shipped) as left, Image.open(rebuilt) as right:
        left.load()
        right.load()
        return (left.format == right.format == 'PNG' and left.mode == right.mode and left.size == right.size and
                getattr(left, 'n_frames', 1) == getattr(right, 'n_frames', 1) == 1 and
                left.info == right.info and left.getpalette() == right.getpalette() and
                left.tobytes() == right.tobytes())


def compare_assets(breed, model, groom=None):
    """Pin authored data, allowing text line endings and lossless PNG encoding changes."""
    checked = []
    png_encoding_differences = []
    maximum = compare_mesh(ASSETS / 'Models' / breed / (breed + '.gltf'), model / (breed + '.gltf'))
    for shipped in sorted((ASSETS / 'Models' / breed).glob(breed + '*')):
        if shipped.suffix not in ('.gltf', '.bin', '.png', '.json', '.oloskin', '.abc'):
            continue
        rebuilt = model / shipped.name
        if shipped.name in (breed + '.bin', breed + '.gltf'):
            equal = True  # Fully checked above, including metadata and every non-morph byte.
        elif shipped.suffix in ('.gltf', '.json'):
            equal = json.loads(shipped.read_text()) == json.loads(rebuilt.read_text())
        elif shipped.suffix == '.oloskin':
            equal = shipped.read_text() == rebuilt.read_text()
        elif shipped.suffix == '.png':
            equal = sha256(shipped) == sha256(rebuilt)
            if not equal and same_png_texels(shipped, rebuilt):
                equal = True
                png_encoding_differences.append(shipped.relative_to(ASSETS).as_posix())
        else:
            equal = sha256(shipped) == sha256(rebuilt)
        if not equal:
            raise ValueError(f'Rebuild differs from the approved asset: {shipped.name}')
        checked.append(shipped.relative_to(ASSETS).as_posix())
    if groom is not None:
        for suffix in ('.ologroom', '.ologroombinding'):
            shipped = ASSETS / 'Grooms' / breed / (breed + suffix)
            if sha256(shipped) != sha256(groom / shipped.name):
                raise ValueError(f'Native cook differs from the approved asset: {shipped.name}')
            checked.append(shipped.relative_to(ASSETS).as_posix())
    return dict(assets=checked, maximumMorphRoundoff=maximum, pngEncodingDifferences=png_encoding_differences)


def rebuild(breed, output, test_exe, mesh_only, verify):
    output = output.resolve()
    if output.exists():
        raise ValueError('Choose a new output directory; existing sources and rebuilds are never overwritten.')
    if not mesh_only and not test_exe.is_file():
        raise FileNotFoundError(f'Build OloEngine-Tests in Release first, or pass --test-exe: {test_exe}')
    output.mkdir(parents=True)
    stage_root = output / 'stages'
    stage_root.mkdir()
    source = stage_root / '00-source'
    source.mkdir()
    inputs = {Path(__file__).resolve(), SOURCES.parent / 'requirements.txt'}
    # These maps and the metadata-complete Alembic archive ARE editable sources.
    # Final mesh/rig/native groom outputs are deliberately excluded from the seed.
    installed = ASSETS / 'Models' / breed
    for path in installed.iterdir():
        if path.name.startswith(breed) and (path.suffix in ('.png', '.abc', '.oloskin') or 'Eyeball.' in path.name):
            shutil.copy2(path, source / path.name)
            inputs.add(path)
    for path in (SOURCES / breed).iterdir():
        shutil.copy2(path, source / path.name)
        inputs.add(path)

    recipe_dir = ASSETS / 'Models' / breed
    sys.path.insert(0, str(recipe_dir))
    if breed == 'Samoyed':
        stages = importlib.import_module('refine_samoyed').STAGES
        inputs.update((SOURCES / 'SamoyedLidSource').iterdir())
        inputs.add(recipe_dir / 'build_samoyed.py')
    else:
        stages = [importlib.import_module(name).build for name in (
            'build_bernese', 'refine_bernese', 'refine_bernese_face',
            'refine_bernese_expression', 'refine_bernese_blink', 'refine_bernese_mouth')]
        inputs.add(recipe_dir / 'authoring_maps.py')
    inputs.update(Path(sys.modules[stage.__module__].__file__).resolve() for stage in stages)
    for index, stage in enumerate(stages, 1):
        target = stage_root / f'{index:02d}-{stage.__module__.split(".")[-1]}-{stage.__name__}'
        print(f'[{breed}] stage {index}/{len(stages)}: {stage.__name__}', flush=True)
        if breed == 'Samoyed':
            stage(source, target, SOURCES / 'SamoyedLidSource')
        else:
            stage(source, target)
        source = target

    model = output / 'Assets/Models' / breed
    model.mkdir(parents=True)
    for path in source.iterdir():
        if path.name.startswith(breed) and path.suffix in ('.gltf', '.bin', '.png', '.json', '.oloskin', '.abc'):
            shutil.copy2(path, model / path.name)
    scene_dir = output / 'Assets/Scenes'
    scene_dir.mkdir()
    scene_source = ASSETS / 'Scenes' / (breed + '.olo')
    shutil.copy2(scene_source, scene_dir / (breed + '.olo'))
    inputs.add(scene_source)
    # This is an asset overlay for the existing checkout, not a standalone
    # project. The separately authored scene retains that project's handles.
    shutil.copytree(ASSETS / 'Materials', output / 'Assets/Materials')
    inputs.update(p for p in (ASSETS / 'Materials').rglob('*') if p.is_file())

    groom = None
    if not mesh_only:
        groom = output / 'Assets/Grooms' / breed
        env = os.environ.copy()
        for name in ('OLO_DOG_EXPORT', 'OLO_DOG_ABC', 'OLO_DOG_BREED', 'OLO_DOG_COOK_DIR', 'OLO_DOG_ASSET_ROOT'):
            env.pop(name, None)
        env.update(OLO_DOG_BREED=breed, OLO_DOG_ASSET_ROOT=str(output / 'Assets'),
                   OLO_DOG_ABC=str(model / (breed + '.abc')), OLO_DOG_COOK_DIR=str(groom))
        log = output / 'native-cook.log'
        with log.open('w') as stream:
            subprocess.run([str(test_exe), '--gtest_filter=DogShowcaseEvidenceTest.CooksTheAuthoredCoatAndBinding'],
                           cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
        if not (groom / (breed + '.ologroom')).is_file() or not (groom / (breed + '.ologroombinding')).is_file():
            raise RuntimeError(f'Native cook produced no pair (a skipped GPU fixture is not a cook): {log}')
    checked = compare_assets(breed, model, groom) if verify else dict(assets=[], maximumMorphRoundoff=None)
    outputs = list(model.iterdir()) + list(scene_dir.iterdir())
    if groom is not None:
        outputs.extend(groom.iterdir())
    manifest = dict(breed=breed, nativeCookComplete=not mesh_only, verifiedAgainstApproved=checked,
                    sources={p.relative_to(ROOT).as_posix(): sha256(p)
                             for p in sorted(inputs) if p.is_file()},
                    nativeCookExecutable=None if mesh_only else dict(path=str(test_exe), sha256=sha256(test_exe)),
                    outputs={p.relative_to(output).as_posix(): sha256(p)
                             for p in sorted(outputs) if p.is_file()})
    (output / 'rebuild.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(output=str(output), nativeCookComplete=not mesh_only, approvedAssetsVerified=len(checked['assets']),
                         maximumMorphRoundoff=checked['maximumMorphRoundoff'])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--breed', choices=('Samoyed', 'Bernese'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--test-exe', type=Path,
                        default=ROOT / 'build-cached/OloEngine/tests/Release/OloEngine-Tests.exe')
    parser.add_argument('--mesh-only', action='store_true')
    parser.add_argument('--verify-shipped', action='store_true')
    args = parser.parse_args()
    rebuild(args.breed, args.output, args.test_exe.resolve(), args.mesh_only, args.verify_shipped)

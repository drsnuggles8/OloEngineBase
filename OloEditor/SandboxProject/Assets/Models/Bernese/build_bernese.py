"""Fair the Bernese upper muzzle from an explicit, immutable glTF source.

python build_bernese.py --source DIRECTORY --output NEW_DIRECTORY

This localized appearance edit adds a static morph. Base surface, skin weights,
UVs, bones, existing animation and full coat source remain unchanged, preserving
the native groom binding. It is not a from-scratch breed generator.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np


class Gltf:
    TYPES = {5121: 'u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
    WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

    def __init__(self, path):
        self.doc = json.loads(path.read_text())
        assert len(self.doc['buffers']) == 1
        self.data = bytearray((path.parent / self.doc['buffers'][0]['uri']).read_bytes())

    def read(self, index):
        accessor = self.doc['accessors'][index]
        view = self.doc['bufferViews'][accessor['bufferView']]
        dtype = np.dtype(self.TYPES[accessor['componentType']])
        width = self.WIDTHS[accessor['type']]
        return np.ndarray((accessor['count'], width), dtype=dtype, buffer=self.data,
                          offset=view.get('byteOffset', 0) + accessor.get('byteOffset', 0),
                          strides=(view.get('byteStride', dtype.itemsize * width), dtype.itemsize)).copy()

    def append(self, values, kind):
        values = np.asarray(values, dtype='<f4').reshape(-1, self.WIDTHS[kind])
        self.data.extend(b'\0' * (-len(self.data) % 4))
        view = len(self.doc['bufferViews'])
        self.doc['bufferViews'].append(dict(buffer=0, byteOffset=len(self.data), byteLength=values.nbytes))
        self.data.extend(values.tobytes())
        accessor = dict(bufferView=view, componentType=5126, count=len(values), type=kind,
                        min=values.min(0).tolist(), max=values.max(0).tolist())
        self.doc['accessors'].append(accessor)
        return len(self.doc['accessors']) - 1


def ramp(lo, hi, x):
    t = np.clip((x - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-20)


def build(source, output):
    source, output = source.resolve(), output.resolve()
    if source == output or output.exists():
        raise ValueError('Use a new output directory; never overwrite a source or candidate.')
    asset = Gltf(source / 'Bernese.gltf')
    assert len(asset.doc['meshes']) == 1
    mesh = asset.doc['meshes'][0]
    assert not mesh.get('weights'), 'This recipe expects the frozen pre-morph Bernese.'
    parts = []
    for primitive in mesh['primitives']:
        parts.append((asset.read(primitive['attributes']['POSITION']).astype(float),
                      asset.read(primitive['attributes']['NORMAL']).astype(float),
                      asset.read(primitive['indices']).reshape(-1, 3).astype(int),
                      asset.doc['materials'][primitive['material']]['name']))
    offsets = np.r_[0, np.cumsum([len(p[0]) for p in parts])]
    all_positions = np.concatenate([p[0] for p in parts])
    _, unique, inverse = np.unique(np.round(all_positions, 7), axis=0, return_index=True, return_inverse=True)
    p = all_positions[unique]
    faces = np.concatenate([inverse[f + offset] for (v, n, f, name), offset in zip(parts, offsets) if name == 'DogSkin'])
    edges = np.unique(np.sort(np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]]), axis=1), axis=0)
    a = np.r_[edges[:, 0], edges[:, 1]]
    b = np.r_[edges[:, 1], edges[:, 0]]
    x, y, z = abs(p[:, 0]), p[:, 1], p[:, 2]
    # Blend the sculpted ridge into the bridge, clear of nose, lip seam and eyes.
    weight = (ramp(.611, .630, y) * (1-ramp(.650, .673, y)) *
              ramp(.505, .535, z) * (1-ramp(.042, .062, x)))
    for offset, (v, n, f, name) in zip(offsets, parts):
        if name != 'DogSkin':
            weight[inverse[offset + np.unique(f)]] = 0
    degree = np.bincount(a, minlength=len(p))
    weight[degree == 0] = 0
    degree = np.maximum(degree, 1)
    q = p.copy()
    for _ in range(16):
        for axis in range(3):
            mean = np.bincount(a, weights=q[b, axis], minlength=len(p)) / degree
            q[:, axis] += .30 * weight * (mean-q[:, axis])
    delta = q-p
    print('Maximum muzzle change (mm):', np.linalg.norm(delta, axis=1).max()*1000)
    assert .0001 < np.linalg.norm(delta, axis=1).max() < .006

    def normals(v):
        face = np.cross(v[faces[:, 1]]-v[faces[:, 0]], v[faces[:, 2]]-v[faces[:, 0]])
        n = np.zeros_like(v)
        for corner in range(3):
            np.add.at(n, faces[:, corner], face)
        return normalize(n)

    old_normal, new_normal = normals(p), normals(q)
    rotation_axis = np.cross(old_normal, new_normal)
    cosine = np.sum(old_normal*new_normal, axis=1)
    rows = []
    for offset, primitive, (v, n, f, name) in zip(offsets, mesh['primitives'], parts):
        ids = inverse[offset:offset+len(v)]
        d = delta[ids] if name == 'DogSkin' else np.zeros_like(v)
        axis = rotation_axis[ids]
        nn = n + np.cross(axis, n) + np.cross(axis, np.cross(axis, n))/np.maximum(1+cosine[ids, None], 1e-8)
        dn = nn-n if name == 'DogSkin' else np.zeros_like(n)
        old = np.cross(v[f[:, 1]]-v[f[:, 0]], v[f[:, 2]]-v[f[:, 0]])
        changed = (np.linalg.norm(d[f], axis=2).max(1) > 1e-8) & (np.linalg.norm(old, axis=1) > 1e-12)
        vv = v+d
        new = np.cross(vv[f[:, 1]]-vv[f[:, 0]], vv[f[:, 2]]-vv[f[:, 0]])
        alignment = np.sum(normalize(old)*normalize(new), axis=1)
        assert not changed.any() or alignment[changed].min() > .70, (name, alignment[changed].min())
        assert not np.count_nonzero(d[(v[:, 1] < .611) | (v[:, 1] > .673)])
        primitive['targets'] = [dict(POSITION=asset.append(d, 'VEC3'), NORMAL=asset.append(dn, 'VEC3'))]
        rows.append(dict(material=name, movedVertices=int(np.count_nonzero(np.linalg.norm(d, axis=1)>1e-8)), maximumMM=float(np.linalg.norm(d, axis=1).max()*1000)))
    mesh['weights'] = [1.0]
    mesh.setdefault('extras', {})['targetNames'] = ['BerneseSoftNasalBridge']
    node = next(i for i, n in enumerate(asset.doc['nodes']) if n.get('mesh') == 0)
    for animation in asset.doc['animations']:
        times = np.unique(np.concatenate([asset.read(s['input']).ravel() for s in animation['samplers']]))
        sampler = len(animation['samplers'])
        animation['samplers'].append(dict(input=asset.append([times[0], times[-1]], 'SCALAR'),
                                         output=asset.append([1., 1.], 'SCALAR'), interpolation='LINEAR'))
        animation['channels'].append(dict(sampler=sampler, target=dict(node=node, path='weights')))
    shutil.copytree(source, output)
    asset.doc['buffers'][0].update(uri='Bernese.bin', byteLength=len(asset.data))
    (output / 'Bernese.gltf').write_text(json.dumps(asset.doc, indent=2)+'\n')
    (output / 'Bernese.bin').write_bytes(asset.data)
    proof = dict(source=str(source), sourceGltfSHA256=hashlib.sha256((source/'Bernese.gltf').read_bytes()).hexdigest(),
                 rows=rows, baseAttributesAndSkinExact=True, existingAnimationChannelsExact=True,
                 coatSourceExact=True, nativeBindingCompatible=True)
    (output / 'bernese-edit.json').write_text(json.dumps(proof, indent=2)+'\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.source, args.output)

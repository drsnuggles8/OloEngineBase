"""Round the Bernese lower muzzle from the seven-target face checkpoint.

Adds a local static morph; the full coat, base binding surface, eyes, body,
existing expressions and animation samples are retained exactly.
"""
from pathlib import Path
import argparse
import json
import shutil

import numpy as np
from build_bernese import Gltf, ramp, normalize


def sculpt(p):
    q = p.copy()
    x, y, z = abs(p[:, 0]), p[:, 1], p[:, 2]
    lower = (ramp(.450, .482, z) * ramp(.520, .553, y) *
             (1-ramp(.580, .600, y)) * (1-ramp(.060, .076, x)))
    # A rounded lip hangs between the front of the muzzle and its corner.
    # The previous rearward lift alone left a straight diagonal shelf.
    lobe = np.exp(-((z-.545)/.031)**2)
    side = .75 + .25*ramp(.007, .027, x)
    front = ramp(.555, .589, z)
    rear = 1-ramp(.487, .520, z)
    q[:, 1] += lower * (.0025*front + .0025*rear - .006*lobe*side)
    # Roll the front underside back beneath the nose, with a small central
    # rise between the lips. This keeps the chin from reading as a square box.
    q[:, 2] -= .004*lower*ramp(.547, .588, z)
    centre = (1-ramp(.005, .023, x))*ramp(.552, .578, z)
    q[:, 1] += .001*lower*centre
    q[:, 0] *= 1-.055*lower*front
    return q


def build(source, output):
    if output.exists() or source.resolve() == output.resolve():
        raise ValueError('Output must be a new directory.')
    asset = Gltf(source/'Bernese.gltf')
    mesh = asset.doc['meshes'][0]
    assert len(mesh['weights']) == 7
    assert mesh['extras']['targetNames'][-1] == 'BerneseBlinkClosed'
    rows = []
    for primitive in mesh['primitives']:
        attr = primitive['attributes']
        p = asset.read(attr['POSITION']).astype(float)
        n = asset.read(attr['NORMAL']).astype(float)
        for weight, target in zip(mesh['weights'], primitive['targets']):
            p += weight*asset.read(target['POSITION'])
            n += weight*asset.read(target['NORMAL'])
        n = normalize(n)
        material = asset.doc['materials'][primitive['material']]['name']
        delta = sculpt(p)-p
        eps = 1e-5
        jac = np.stack([(sculpt(p+a*eps)-sculpt(p-a*eps))/(2*eps)
                        for a in np.eye(3)], axis=2)
        determinant = np.linalg.det(jac)
        assert determinant.min() > .30, (material, determinant.min())
        nn = normalize(np.linalg.solve(jac.transpose(0, 2, 1), n[..., None])[..., 0])
        dn = nn-n
        tri = asset.read(primitive['indices']).reshape(-1, 3).astype(int)
        if material == 'DogTeeth':
            # Translate each tooth with the local gum; never bend enamel.
            _, inverse = np.unique(np.round(p, 6), axis=0, return_inverse=True)
            parent = np.arange(inverse.max()+1)
            def root(i):
                while parent[i] != i:
                    parent[i] = parent[parent[i]]
                    i = parent[i]
                return i
            for face in inverse[tri]:
                for vertex in face[1:]:
                    parent[root(vertex)] = root(face[0])
            groups = np.array([root(i) for i in inverse])
            for group in np.unique(groups):
                selected = groups == group
                centre = p[selected].mean(0, keepdims=True)
                delta[selected] = sculpt(centre)-centre
            dn[:] = 0
        q = p+delta
        before = np.cross(p[tri[:, 1]]-p[tri[:, 0]], p[tri[:, 2]]-p[tri[:, 0]])
        after = np.cross(q[tri[:, 1]]-q[tri[:, 0]], q[tri[:, 2]]-q[tri[:, 0]])
        valid = np.linalg.norm(before, axis=1) > 1e-12
        alignment = float(np.sum(normalize(before)*normalize(after), axis=1)[valid].min())
        assert alignment > .65, (material, alignment)
        if material != 'DogTeeth':
            assert np.allclose(delta[(p[:, 1] < .520) | (p[:, 1] > .600)], 0, atol=1e-12)
        if material in ('DogNose', 'DogLid'):
            assert np.allclose(delta, 0, atol=1e-12)
        primitive['targets'].append(dict(POSITION=asset.append(delta, 'VEC3'),
                                         NORMAL=asset.append(dn, 'VEC3')))
        rows.append(dict(material=material, maximumMM=float(np.linalg.norm(delta, axis=1).max()*1000),
                         minimumJacobian=float(determinant.min()), minimumTriangleAlignment=alignment))
    mesh['weights'].append(1.)
    mesh['extras']['targetNames'].append('BerneseRoundedLipAndChin')
    for animation in asset.doc['animations']:
        for channel in animation['channels']:
            if channel['target']['path'] == 'weights':
                sampler = animation['samplers'][channel['sampler']]
                old = asset.read(sampler['output']).reshape(-1, 7)
                sampler['output'] = asset.append(np.c_[old, np.ones(len(old))].ravel(), 'SCALAR')
    shutil.copytree(source, output, ignore=shutil.ignore_patterns('__pycache__'))
    asset.doc['buffers'][0].update(uri='Bernese.bin', byteLength=len(asset.data))
    (output/'Bernese.gltf').write_text(json.dumps(asset.doc, indent=2)+'\n')
    (output/'Bernese.bin').write_bytes(asset.data)
    report = dict(parent=source.name, rows=rows, baseBindingSurfaceExact=True,
                  existingSevenMorphTargetsExact=True, existingAnimationSamplesExact=True,
                  eyesNoseBodyLegsAndCoatExact=True)
    (output/'bernese-mouth-refinement.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(args.source, args.output)

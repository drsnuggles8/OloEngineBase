"""Re-solve a breed's front legs in its Walk clip so the paws stay planted, and set the walk's travel.

Example (both copies of the Samoyed, the editable source and the shipped model):

    python tools/dog-authoring/retarget_front_walk.py --breed Samoyed --speed 0.312 \
        tools/dog-authoring/sources/Samoyed OloEditor/SandboxProject/Assets/Models/Samoyed

WHY. The Walk clip's rotations are the golden's (build_dog.py's clip_walk), authored for a leg whose
joints lie along each bone. The Samoyed's front legs are not built that way: each forearm sits 59 mm
off its upper arm's axis and each upper arm 17 mm off its shoulder's. The golden's rotations then
swing the paw on another arc: it sank 20 mm through the stance and pushed off 18 mm into the ground.
Its legs are the golden's length, so its paws travel at the golden's 0.312 m/s, while the clip moved
the root at 0.353 m/s (WALK_SPEED times a stride the warp over-stated): every paw slid 19 mm a stance.
DogShowcaseEvidenceTest's TheWalkTravelsWithItsPawsPlanted holds a walk to 8 mm and to no dip.

WHAT. The root travels at --speed. Each front leg keeps its own path along the body (which that travel
keeps planted) and takes the golden's paw height at the same instant, offset to the breed's rest paw;
the upper arm and forearm flex about their hinges (local x) to reach it, warm-started key to key so
the elbow keeps one branch, and the paw keeps the golden's world rotation relative to its rest. Where
the leg cannot reach the golden's height (landing, fully extended), it stops at the closest pose with
its path along the body held. Only the Walk clip's front-leg rotations and root translation change, in
place: the buffer's layout, every other clip and the rest pose are untouched, so a source rebuild
(rebuild.py --verify-shipped) still reproduces the shipped model. Write the same --speed into the
breed's <breed>.rig.json walkSpeed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / 'OloEditor/SandboxProject/Assets/Models/Dog'


def accessor_span(g, i):
    a = g['accessors'][i]
    view = g['bufferViews'][a['bufferView']]
    width = {'SCALAR': 1, 'VEC3': 3, 'VEC4': 4}[a['type']]
    if a['componentType'] != 5126 or view.get('byteStride', 4 * width) != 4 * width:
        raise ValueError(f'accessor {i} is not a tightly packed float array')
    return view.get('byteOffset', 0) + a.get('byteOffset', 0), a['count'], width


def read(g, data, i):
    offset, count, width = accessor_span(g, i)
    return np.frombuffer(bytes(data[offset:offset + 4 * count * width]), '<f4').reshape(count, width).astype(np.float64)


def write(g, data, i, values):
    offset, count, width = accessor_span(g, i)
    data[offset:offset + 4 * count * width] = np.asarray(values, dtype='<f4').reshape(count, width).tobytes()


def quat_to_matrix(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def matrix_to_quat(m):
    trace = np.trace(m)
    if trace > 0:
        s = 0.5 / np.sqrt(trace + 1.0)
        q = [(m[2, 1] - m[1, 2]) * s, (m[0, 2] - m[2, 0]) * s, (m[1, 0] - m[0, 1]) * s, 0.25 / s]
    else:
        i = int(np.argmax(np.diag(m)))
        if i == 0:
            s = 2.0 * np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2])
            q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
        elif i == 1:
            s = 2.0 * np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2])
            q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
        else:
            s = 2.0 * np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1])
            q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def hinge(angle):
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]])


class Clip:
    """A glTF rig and one clip's sampled tracks; world transforms at a key, with rotation overrides."""

    def __init__(self, directory, name, clip='Walk'):
        self.path = Path(directory) / (name + '.gltf')
        with self.path.open(encoding='utf-8', newline='') as stream:
            self.text = stream.read()  # as written, line endings included
        self.g = json.loads(self.text)
        self.bin = Path(directory) / self.g['buffers'][0]['uri']
        self.data = bytearray(self.bin.read_bytes())
        self.nodes = self.g['nodes']
        self.index = {n['name']: i for i, n in enumerate(self.nodes)}
        self.parent = [-1] * len(self.nodes)
        for i, n in enumerate(self.nodes):
            for c in n.get('children', []):
                self.parent[c] = i
        animation = next(a for a in self.g['animations'] if a['name'] == clip)
        self.tracks = {}
        for channel in animation['channels']:
            sampler = animation['samplers'][channel['sampler']]
            key = (self.nodes[channel['target']['node']]['name'], channel['target']['path'])
            self.tracks[key] = [sampler['output'], read(self.g, self.data, sampler['output'])]

    def local(self, name, k, override, animated):
        node = self.nodes[self.index[name]]
        t = np.array(node.get('translation', [0, 0, 0]), float)
        r = quat_to_matrix(np.array(node.get('rotation', [0, 0, 0, 1]), float))
        s = np.array(node.get('scale', [1, 1, 1]), float)
        if animated:
            for path in ('translation', 'rotation', 'scale'):
                if (name, path) in self.tracks:
                    values = self.tracks[(name, path)][1]
                    v = values[min(k, len(values) - 1)]
                    t, r, s = (v, r, s) if path == 'translation' else (t, quat_to_matrix(v), s) if path == 'rotation' else (t, r, v)
        if override and name in override:
            r = override[name]
        m = np.eye(4)
        m[:3, :3] = r * s[None, :]
        m[:3, 3] = t
        return m

    def world(self, name, k, override=None, animated=True):
        i = self.index[name]
        m = self.local(name, k, override, animated)
        p = self.parent[i]
        while p >= 0:
            m = self.local(self.nodes[p]['name'], k, override, animated) @ m
            p = self.parent[p]
        return m


def retarget(directory, breed, speed):
    golden = Clip(GOLDEN, 'Dog')
    clip = Clip(directory, breed)
    root_index, root = clip.tracks[('root', 'translation')]
    keys = len(clip.tracks[('upperarm_L', 'rotation')][1])
    if len(root) != keys:
        raise ValueError('the root translation and the front legs are not keyed alike')
    # The new travel, rounded as the buffer stores it, and each front paw's path along the body under it.
    seconds = float(read(clip.g, clip.data, next(a['samplers'][c['sampler']]['input'] for a in clip.g['animations']
                                                 if a['name'] == 'Walk' for c in a['channels']
                                                 if clip.nodes[c['target']['node']]['name'] == 'root'
                                                 and c['target']['path'] == 'translation'))[-1, 0])
    along = np.linspace(0.0, 1.0, keys)[:, None] * np.array([0.0, 0.0, speed * seconds])
    travel = (root[0] + along).astype(np.float32).astype(np.float64)
    paths = {side: [clip.world(f'paw_front_{side}', k)[:3, 3] + (travel[k] - root[k]) for k in range(keys)]
             for side in ('L', 'R')}
    clip.tracks[('root', 'translation')][1] = travel
    write(clip.g, clip.data, root_index, travel)
    weights = np.array([1.0, 1.0, 10.0])  # the path along the body is held hard; the height where reached
    for side in ('L', 'R'):
        upper, fore, wrist, paw = f'upperarm_{side}', f'forearm_{side}', f'wrist_{side}', f'paw_front_{side}'
        golden_rest = golden.world(paw, 0, animated=False)
        breed_rest = clip.world(paw, 0, animated=False)
        lift = breed_rest[1, 3] - golden_rest[1, 3]
        solved = {upper: [], fore: [], paw: []}
        angles = np.zeros(2)
        for k in range(keys):
            target = paths[side][k].copy()
            target[1] = golden.world(paw, k)[1, 3] + lift
            ru = quat_to_matrix(clip.tracks[(upper, 'rotation')][1][k])
            rf = quat_to_matrix(clip.tracks[(fore, 'rotation')][1][k])

            def reach(a):
                return weights * clip.world(paw, k, {upper: ru @ hinge(a[0]), fore: rf @ hinge(a[1])})[:3, 3]

            for _ in range(200):
                residual = reach(angles) - weights * target
                if np.linalg.norm(residual) < 1e-7:
                    break
                jacobian = np.zeros((3, 2))
                for j in range(2):
                    e = np.zeros(2)
                    e[j] = 1e-5
                    jacobian[:, j] = (reach(angles + e) - reach(angles - e)) / 2e-5
                step = np.linalg.solve(jacobian.T @ jacobian + 1e-4 * np.eye(2), jacobian.T @ residual)
                angles = angles - step * min(1.0, 0.2 / max(np.abs(step).max(), 1e-12))
            override = {upper: ru @ hinge(angles[0]), fore: rf @ hinge(angles[1])}
            turn = golden.world(paw, k)[:3, :3] @ golden_rest[:3, :3].T
            parent = clip.world(wrist, k, override)[:3, :3]
            solved[upper].append(matrix_to_quat(override[upper]))
            solved[fore].append(matrix_to_quat(override[fore]))
            solved[paw].append(matrix_to_quat(parent.T @ turn @ breed_rest[:3, :3]))
        for name, quats in solved.items():
            quats = np.array(quats)
            for i in range(1, len(quats)):  # one hemisphere, for LINEAR interpolation
                if np.dot(quats[i], quats[i - 1]) < 0:
                    quats[i] = -quats[i]
            write(clip.g, clip.data, clip.tracks[(name, 'rotation')][0], quats)
    # The root accessor's bounds, patched in the file's own text so nothing else in it moves.
    accessor = clip.g['accessors'][root_index]
    text = clip.text
    for bound, value in (('min', travel.min(0)), ('max', travel.max(0))):
        for old, new in zip(accessor[bound], value.tolist()):
            if old != new:
                if text.count(repr(old)) != 1:
                    raise ValueError(f'cannot place the root accessor {bound} {old!r} in {clip.path}')
                text = text.replace(repr(old), repr(new))
    clip.bin.write_bytes(clip.data)
    with clip.path.open('w', encoding='utf-8', newline='') as stream:
        stream.write(text)
    print(f'{clip.path}: root travels {travel[-1, 2] - travel[0, 2]:.6f} m in {seconds:g} s; front legs re-solved')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--breed', required=True)
    parser.add_argument('--speed', type=float, required=True, help='the walk travel, m/s')
    parser.add_argument('directories', nargs='+', type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        retarget(directory, args.breed, args.speed)

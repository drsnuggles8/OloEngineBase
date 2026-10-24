"""The play clips (#1559) for every furred dog, from one recipe: fetch, petting, and the life between.

Example: the golden's shipped model in place (build_dog.py also runs this after its export), then
each breed's editable source, rebuilt and installed:

    python tools/dog-authoring/dog_clips.py OloEditor/SandboxProject/Assets/Models/Dog/Dog.gltf
    python tools/dog-authoring/dog_clips.py tools/dog-authoring/sources/Bernese/Bernese.gltf         tools/dog-authoring/sources/Samoyed/Samoyed.gltf
    python tools/dog-authoring/rebuild.py --breed Bernese --output C:/temp/bernese --mesh-only
    (copy C:/temp/bernese/Assets/Models/Bernese/Bernese.gltf, .bin and .rig.json over the shipped three)

WHAT. Six clips, appended after the glTF's own animations so the existing clip indices stay valid:
Run (a rotary gallop with root motion), PickUp (head down, the jaw closes on a ball on the ground),
Carry (a trot, the jaw at the hold opening), Drop (the jaw lets go), PlayBow and Petted (eyes half
shut). The rig description (<model>.rig.json beside the glTF) gets the clips, `runSpeed`,
`carrySpeed`, the grab and release instants (`events`, normalised clip time), the `sockets` a held
toy hangs from (jaw-local glTF transforms; a stick's long axis is its local X) and `fetch`: the hold
opening and where PickUp expects the ball (model space).

Thirteen more follow them, the vocabulary a mood-driven dog plays from: Trot and Sniff (gaits with
root motion, `trotSpeed` and `sniffSpeed`), Shake, Lie (lying sternal, a loop) with LieDown and GetUp
into and out of it, BellyUp (on its back, a loop) with RollOver from Lie into it, Excited, Scratch,
Stretch, Nudge (its `nudge` event the push's peak) and TurnInPlace (footwork only: the script turns
the root). A transition starts and ends EXACTLY on the frame it joins: Idle's first (a stand), Sit's
last, Lie's first, BellyUp's first (Pose.start_from, Pose.blend_to). Lying, rolling and
scratching legs are set outright from their bones' lengths (fold_leg) rather than solved, so an elbow never
flips; a body that lies on the grass rests on it (settle), and a tail, an ear or a head that would
sink into it drapes on it instead (chain_on_ground, head_down).

One of the glTF's own clips is re-authored here, where it stands (REAUTHORED, its index and rig entry
kept): Sit, build_dog.py's recipe with its contacts solved. Its legs were angles, and on every breed
the rump hung 53 to 103 mm over the grass while the hind paws went up to 30 mm into it, the forepaws
slid back and sank (15 mm on the golden, 44 on the Samoyed) and the Bernese's tail went 110 mm in.

HOW. A clip is a sampler(pose, t) over phase t in [0, 1], baked at 30 keys a second like build_dog.py's
(the gallop, the trot and the quick shakes and strokes at 60, KEY_RATE). Its rotations are about
ARMATURE axes (build_dog.py's convention: +X the dog's left, a positive turn pitches a bone's forward
end down; +Y toward the tail; +Z up), applied to each bone as rest^-1 * q * rest, so every breed's
own rest pose carries the same motion. Contacts are
SOLVED per breed rather than authored as angles: each leg reaches its paw's target (planted where it
stands, or on its stride) by damped least squares over its hinge pitches, a gait's stance spans the
ground that breed's leg reaches (Dog.envelope), and PickUp's neck and head put the held ball on the
ball (Dog.pickup_plan). A breed whose foreleg sits off its bones' axes (the Samoyed's elbow,
retarget_front_walk.py) therefore plants its paws as the golden does. Distances scale with the
breed's leg (Dog.scale), so a Bernese gallops at 3.8 m/s where the golden does 3.0.

Every clip keys every joint the existing clips key, translation, rotation and scale (LINEAR where it
moves, a two-key STEP where it holds, as Blender's exporter writes them), so a blend always sees a full
pose. Breed stages are followed, not copied:
- the lids are keyed as bones (golden; the Bernese stages turn them into blink morphs, and
  refine_bernese_expression.py reads a full blink as 41 degrees of upper lid);
- a glTF whose clips carry the Samoyed's morph `weights` channel and no lid channels gets that channel:
  blink columns from the lid amount through build_samoyed.basis_weights, MouthSeal from the jaw angle
  as build_samoyed.py derives it, the other sculpts as its Rest clip holds them;
- a rest carriage the existing clips add on top of every pose (the Samoyed's lay_tail plume) is read
  off the glTF's Rest clip and added to these clips too;
- the mouth (hold opening, sockets) is solved on the FINISHED model's teeth, the installed one a
  breed source rebuilds into (teeth_model): the stages reshape the jaw after the clips.

Running it again replaces these clips (and their trailing buffer data) instead of adding copies.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
MODELS = ROOT / 'OloEditor/SandboxProject/Assets/Models'
FPS = 30
NAMES = ('Run', 'PickUp', 'Carry', 'Drop', 'PlayBow', 'Petted', 'Trot', 'Sniff', 'Shake', 'Lie', 'LieDown', 'GetUp',
         'BellyUp', 'RollOver', 'Excited', 'Scratch', 'Stretch', 'Nudge', 'TurnInPlace')
TYPES = {5121: 'u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
# The JSON layouts the dogs' glTFs are written in: Blender's exporter (the golden, the Bernese source)
# and the breed stages' json.dumps(indent=2). A rewrite keeps the file's own, so its diff is the clips.
STYLES = (dict(indent='\t', separators=(',', ':')), dict(indent=2), dict(indent=4))

AX_X, AX_Y, AX_Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)

# ----------------------------------------------------------------------------
# Quaternions (glTF's x, y, z, w) and frames
# ----------------------------------------------------------------------------

IDENTITY = np.array([0.0, 0.0, 0.0, 1.0])


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz])


def qinv(q):
    return np.array([-q[0], -q[1], -q[2], q[3]]) / np.dot(q, q)


def qaxis(axis, degrees):
    axis = np.asarray(axis, float)
    axis = axis / np.linalg.norm(axis)
    half = math.radians(degrees) / 2.0
    return np.r_[axis * math.sin(half), math.cos(half)]


def qmat(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def matq(m):
    u, _s, vt = np.linalg.svd(m)  # the nearest rotation: a joint's world matrix carries its scale
    m = u @ vt
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


def qangle(q, rest):
    """The angle (degrees) of the turn from `rest` to `q`, as the breed stages measure a lid or the jaw."""
    q, rest = q / np.linalg.norm(q), rest / np.linalg.norm(rest)
    return math.degrees(2.0 * math.acos(min(1.0, abs(float(np.dot(q, rest))))))


def slerp(a, b, k):
    a, b = np.asarray(a, float), np.asarray(b, float)
    if np.dot(a, b) < 0.0:
        b = -b
    d = min(1.0, float(np.dot(a, b)) / (np.linalg.norm(a) * np.linalg.norm(b)))
    angle = math.acos(d)
    if angle < 1e-6:
        q = a + (b - a) * k
    else:
        q = (math.sin((1.0 - k) * angle) * a + math.sin(k * angle) * b) / math.sin(angle)
    return q / np.linalg.norm(q)


def gl(v):
    """Blender's armature axes (Z up, the dog facing -Y) -> glTF's (Y up, the dog facing +Z)."""
    return np.array([v[0], v[2], -v[1]], float)


def trs(t, q, s):
    m = np.eye(4)
    m[:3, :3] = qmat(q) * np.asarray(s, float)[None, :]
    m[:3, 3] = t
    return m


def rotation(m):
    """The rotation part of a joint's world matrix (exporter scales sit within 1e-6 of one)."""
    r = m[:3, :3]
    return r / np.linalg.norm(r, axis=0, keepdims=True)


def _ease(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def _pulse(t, at, rise, hold=0.0, fall=None):
    """0 -> 1 -> 0 around phase `at` (rise, hold, fall as phase lengths), build_dog.py's."""
    fall = rise if fall is None else fall
    if t < at - rise or t > at + hold + fall:
        return 0.0
    if t < at:
        return _ease((t - (at - rise)) / rise)
    if t <= at + hold:
        return 1.0
    return 1.0 - _ease((t - at - hold) / fall)


def _ramp(t, a, b):
    """0 before a, 1 after b, eased between."""
    return _ease((t - a) / (b - a))


# ----------------------------------------------------------------------------
# glTF
# ----------------------------------------------------------------------------


class Gltf:
    """A glTF with one external buffer, edited in place: accessors are appended, never rewritten."""

    def __init__(self, path):
        self.path = Path(path)
        text = self.path.read_text(encoding='utf-8')
        self.doc = json.loads(text)
        self.style = next((s for s in STYLES if json.dumps(self.doc, **s) + '\n' == text), STYLES[1])
        if len(self.doc['buffers']) != 1:
            raise ValueError(f'{self.path}: expected one buffer')
        self.data = bytearray((self.path.parent / self.doc['buffers'][0]['uri']).read_bytes())
        if len(self.data) != self.doc['buffers'][0]['byteLength']:
            raise ValueError(f'{self.path}: the buffer is {len(self.data)} bytes, the glTF says '
                             f'{self.doc["buffers"][0]["byteLength"]} (a Git LFS pointer? run git lfs pull)')

    def read(self, index):
        a = self.doc['accessors'][index]
        view = self.doc['bufferViews'][a['bufferView']]
        dtype, width = np.dtype(TYPES[a['componentType']]), WIDTHS[a['type']]
        return np.ndarray((a['count'], width), dtype=dtype, buffer=self.data,
                          offset=view.get('byteOffset', 0) + a.get('byteOffset', 0),
                          strides=(view.get('byteStride', dtype.itemsize * width), dtype.itemsize)).copy()

    def append(self, values, kind, bounds=False):
        values = np.ascontiguousarray(np.asarray(values, dtype='<f4').reshape(-1, WIDTHS[kind]))
        self.data.extend(b'\0' * (-len(self.data) % 4))
        self.doc['bufferViews'].append(dict(buffer=0, byteLength=values.nbytes, byteOffset=len(self.data)))
        self.data.extend(values.tobytes())
        accessor = dict(bufferView=len(self.doc['bufferViews']) - 1, componentType=5126, count=len(values), type=kind)
        if bounds:
            accessor.update(max=values.max(0).tolist(), min=values.min(0).tolist())
        self.doc['accessors'].append(accessor)
        return len(self.doc['accessors']) - 1

    def remove_animations(self, names, slots=()):
        """Drop the named animations, and the accessors, views and bytes only they used. Those in
        `slots` (a glTF's own clip this script re-authors) leave an empty animation at their index for
        write_animation to fill. Data at the file's tail (where this script appends) is cut off; data
        elsewhere (a re-authored clip's first replacement) is cut out and every later index renumbered."""
        doomed = [a for a in self.doc.get('animations', []) if a['name'] in names or a['name'] in slots]
        if not doomed:
            return 0
        keep = [a for a in self.doc['animations'] if a['name'] not in names and a['name'] not in slots]
        after = [dict(channels=[], name=a['name'], samplers=[]) if a['name'] in slots else a
                 for a in self.doc['animations'] if a['name'] not in names]
        used = set()
        for mesh in self.doc['meshes']:
            for p in mesh['primitives']:
                used.update(p['attributes'].values())
                used.update(v for t in p.get('targets', []) for v in t.values())
                if 'indices' in p:
                    used.add(p['indices'])
        used.update(s['inverseBindMatrices'] for s in self.doc.get('skins', []) if 'inverseBindMatrices' in s)
        for a in keep:
            for s in a['samplers']:
                used.update((s['input'], s['output']))
        theirs = {i for a in doomed for s in a['samplers'] for i in (s['input'], s['output'])} - used
        if not theirs:
            self.doc['animations'] = after
            return len(doomed)
        first = min(theirs)
        if theirs != set(range(first, len(self.doc['accessors']))):
            self.doc['animations'] = after
            self._cut_accessors(theirs)
            return len(doomed)
        views = {self.doc['accessors'][i]['bufferView'] for i in theirs}
        view_first = min(views)
        others = {a['bufferView'] for i, a in enumerate(self.doc['accessors']) if i < first}
        if views != set(range(view_first, len(self.doc['bufferViews']))) or others & views:
            raise ValueError(f'{self.path}: the replaced clips do not own the trailing buffer views')
        cut = min(self.doc['bufferViews'][v]['byteOffset'] for v in views)
        if any(v.get('byteOffset', 0) + v['byteLength'] > cut for v in self.doc['bufferViews'][:view_first]):
            raise ValueError(f'{self.path}: the replaced clips do not own the end of the buffer')
        del self.doc['accessors'][first:]
        del self.doc['bufferViews'][view_first:]
        del self.data[cut:]
        # The exporter pads the buffer to four bytes; so does append, so the tail before them was padded.
        self.doc['animations'] = after
        return len(doomed)

    def _cut_accessors(self, doomed):
        """Remove these accessors, their views and their bytes from anywhere in the file: each view's
        bytes run to the next view's start (its padding with it), so every view after a cut moves by a
        multiple of four and keeps its alignment. Every accessor and view index is renumbered."""
        doc = self.doc
        views = {doc['accessors'][i]['bufferView'] for i in doomed}
        if any(a.get('bufferView') in views for i, a in enumerate(doc['accessors']) if i not in doomed):
            raise ValueError(f'{self.path}: a view of the replaced clips is shared with other data')
        if any(im.get('bufferView') in views for im in doc.get('images', [])):
            raise ValueError(f'{self.path}: a view of the replaced clips holds an image')
        order = sorted(range(len(doc['bufferViews'])), key=lambda v: doc['bufferViews'][v].get('byteOffset', 0))
        starts = [doc['bufferViews'][v].get('byteOffset', 0) for v in order] + [len(self.data)]
        if any(s % 4 for s in starts[:-1]):
            raise ValueError(f'{self.path}: a buffer view is not four-byte aligned')
        data = bytearray(self.data[:starts[0]])
        moved = {}
        for k, v in enumerate(order):
            if v in views:
                continue
            moved[v] = len(data)
            data.extend(self.data[starts[k]:starts[k + 1]])
        view_map, kept_views = {}, []
        for v, view in enumerate(doc['bufferViews']):
            if v not in views:
                view_map[v] = len(kept_views)
                kept_views.append(dict(view, byteOffset=moved[v]))
        acc_map, kept_accessors = {}, []
        for i, a in enumerate(doc['accessors']):
            if i not in doomed:
                acc_map[i] = len(kept_accessors)
                kept_accessors.append(dict(a, bufferView=view_map[a['bufferView']]))
        for mesh in doc['meshes']:
            for p in mesh['primitives']:
                p['attributes'] = {k: acc_map[v] for k, v in p['attributes'].items()}
                if 'targets' in p:
                    p['targets'] = [{k: acc_map[v] for k, v in t.items()} for t in p['targets']]
                if 'indices' in p:
                    p['indices'] = acc_map[p['indices']]
        for s in doc.get('skins', []):
            if 'inverseBindMatrices' in s:
                s['inverseBindMatrices'] = acc_map[s['inverseBindMatrices']]
        for a in doc['animations']:
            for s in a['samplers']:
                s['input'], s['output'] = acc_map[s['input']], acc_map[s['output']]
        for im in doc.get('images', []):
            if 'bufferView' in im:
                im['bufferView'] = view_map[im['bufferView']]
        doc['accessors'], doc['bufferViews'] = kept_accessors, kept_views
        self.data = data

    def write(self):
        self.doc['buffers'][0]['byteLength'] = len(self.data)
        (self.path.parent / self.doc['buffers'][0]['uri']).write_bytes(self.data)
        self.path.write_text(json.dumps(self.doc, **self.style) + '\n', encoding='utf-8')


# ----------------------------------------------------------------------------
# The skeleton and a pose on it
# ----------------------------------------------------------------------------


class Skeleton:
    def __init__(self, gltf):
        doc = gltf.doc
        self.gltf = gltf
        self.nodes = doc['nodes']
        self.names = [n.get('name', f'node{i}') for i, n in enumerate(self.nodes)]
        self.index = {n: i for i, n in enumerate(self.names)}
        self.parent = [-1] * len(self.nodes)
        for i, n in enumerate(self.nodes):
            for c in n.get('children', []):
                self.parent[c] = i
        self.order = []
        stack = list(reversed(doc['scenes'][doc.get('scene', 0)]['nodes']))
        while stack:
            i = stack.pop()
            self.order.append(i)
            stack.extend(reversed(self.nodes[i].get('children', [])))
        self.rest_t = [np.array(n.get('translation', [0, 0, 0]), float) for n in self.nodes]
        self.rest_q = [np.array(n.get('rotation', [0, 0, 0, 1]), float) for n in self.nodes]
        self.rest_s = [np.array(n.get('scale', [1, 1, 1]), float) for n in self.nodes]
        self.joints = doc['skins'][0]['joints']
        self.rest_world = [None] * len(self.nodes)
        for i in self.order:
            p = self.parent[i]
            local = trs(self.rest_t[i], self.rest_q[i], self.rest_s[i])
            self.rest_world[i] = local if p < 0 else self.rest_world[p] @ local
        self.rest_wq = [matq(rotation(m)) for m in self.rest_world]
        self.rest_wr = [rotation(m) for m in self.rest_world]
        self.ancestors = {}
        for i in range(len(self.nodes)):
            chain = []
            j = i
            while j >= 0:
                chain.append(j)
                j = self.parent[j]
            self.ancestors[i] = chain[::-1]
        # A rest carriage every existing clip adds on top of its pose (see carriage_from_rest_clip).
        self.carriage = {}

    def head(self, name):
        return self.rest_world[self.index[name]][:3, 3].copy()


class Pose:
    """build_dog.py's Rig, on a glTF skeleton: rotations about ARMATURE axes, translations in armature
    space, and (for the solved limbs) a bone's world rotation set outright."""

    def __init__(self, sk):
        self.sk = sk
        self.q = {}
        self.off = {}
        self.world_rot = {}
        self.blink_amount = 0.0
        self._cache = {}
        # A frame of another clip a joint starts from instead of its rest (node -> local t, q, s), and
        # frames the keyed pose is blended toward ([(node -> local t, q, s), weight]): see Frames.
        self.base = {}
        self.anchors = []

    def rot(self, bone, axis, degrees):
        """Rotate `bone` about an ARMATURE-space axis (Blender's axes, build_dog.py's Rig.rot)."""
        self.rot_gl(bone, gl(axis), degrees)

    def rot_gl(self, bone, axis, degrees):
        if degrees:
            self.q[bone] = qmul(self.q.get(bone, IDENTITY), qaxis(axis, degrees))
            self._cache.clear()

    def move(self, bone, offset):
        self.off[bone] = self.off.get(bone, np.zeros(3)) + gl(offset)
        self._cache.clear()

    def orient(self, bone, r):
        """Give `bone` this WORLD rotation (3x3), whatever its parents do."""
        self.world_rot[bone] = np.asarray(r, float)
        self._cache.clear()

    def copy(self):
        p = Pose(self.sk)
        p.q, p.off, p.world_rot, p.blink_amount = dict(self.q), dict(self.off), dict(self.world_rot), self.blink_amount
        p.base, p.anchors = dict(self.base), list(self.anchors)
        return p

    def start_from(self, frame, nodes=None):
        """Start these joints (all a frame keys by default) from a frame of another clip: rotations
        and moves then act on top of it as they act on top of the rest."""
        for i, v in frame.items():
            if nodes is None or self.sk.names[i] in nodes:
                self.base[i] = v
        self._cache.clear()

    def blend_to(self, frame, weight):
        """Blend the keyed pose toward `frame` by `weight` (1: exactly that frame). The legs' solves
        and the checks in the clip functions see the unblended pose: keep blends short and between
        nearly equal poses, or to an exact frame at a clip's end."""
        if weight > 0.0:
            self.anchors.append((frame, min(1.0, weight)))

    def _base_local(self, i, name, parent_world):
        sk = self.sk
        t, q, s = self.base[i]
        if name in self.off:
            p = sk.parent[i]
            t = t + (sk.rest_wr[p].T @ self.off[name] if p >= 0 else self.off[name])
        if name in self.world_rot:
            pr = rotation(parent_world) if parent_world is not None else np.eye(3)
            q = matq(pr.T @ self.world_rot[name])
        elif name in self.q:
            w = sk.rest_wq[i]
            q = qmul(q, qmul(qinv(w), qmul(self.q[name], w)))
        return np.asarray(t, float), q / np.linalg.norm(q), np.asarray(s, float)

    def local(self, i, parent_world):
        sk, name = self.sk, self.sk.names[i]
        if i in self.base:
            return self._base_local(i, name, parent_world)
        t = sk.rest_t[i]
        if name in self.off:
            p = sk.parent[i]
            t = t + (sk.rest_wr[p].T @ self.off[name] if p >= 0 else self.off[name])
        if name in self.world_rot:
            pr = rotation(parent_world) if parent_world is not None else np.eye(3)
            q = matq(pr.T @ self.world_rot[name])
        else:
            q = sk.rest_q[i]
            if name in self.q:
                w = sk.rest_wq[i]
                q = qmul(q, qmul(qinv(w), qmul(self.q[name], w)))
            if i in sk.carriage:
                q = qmul(q, sk.carriage[i])
        return t, q / np.linalg.norm(q), sk.rest_s[i]

    def world(self, bone):
        """The world matrix of one bone (its ancestors only)."""
        i = self.sk.index[bone] if isinstance(bone, str) else bone
        if i in self._cache:
            return self._cache[i]
        m = None
        for j in self.sk.ancestors[i]:
            if j in self._cache:
                m = self._cache[j]
                continue
            local = trs(*self.local(j, m))
            m = local if m is None else m @ local
            self._cache[j] = m
        return m

    def locals(self):
        out = {}
        for i in self.sk.order:
            p = self.sk.parent[i]
            out[i] = self.local(i, self.world(p) if p >= 0 else None)
        for frame, weight in self.anchors:
            for i, (t, q, s) in out.items():
                if i not in frame:
                    continue
                ft, fq, fs = (np.asarray(v, float) for v in frame[i])
                if weight >= 1.0:
                    out[i] = (ft, fq, fs)
                else:
                    out[i] = (t + (ft - t) * weight, slerp(q, fq, weight), s + (fs - s) * weight)
        return out


def carriage_from_rest_clip(gltf, sk):
    """The per-joint rotation every existing clip adds on top of its pose, read off the Rest clip (the
    bind pose held): the Samoyed's lay_tail plume on tail_01 and tail_02; nothing on the golden."""
    rest = next((a for a in gltf.doc.get('animations', []) if a['name'] == 'Rest'), None)
    if rest is None:
        raise ValueError(f'{gltf.path}: no Rest clip to read the rest carriage from')
    carriage = {}
    for ch in rest['channels']:
        node, path = ch['target'].get('node'), ch['target']['path']
        if path == 'weights':
            continue
        value = gltf.read(rest['samplers'][ch['sampler']]['output'])[0].astype(float)
        if path == 'rotation':
            off = qmul(qinv(sk.rest_q[node]), value / np.linalg.norm(value))
            if qangle(off, IDENTITY) > 1e-3:
                carriage[node] = off
        elif np.abs(value - (sk.rest_t[node] if path == 'translation' else sk.rest_s[node])).max() > 1e-6:
            raise ValueError(f'{gltf.path}: the Rest clip moves {sk.names[node]} {path}; only rotations are carried')
    return carriage


# ----------------------------------------------------------------------------
# The skinned body, for the checks and the sockets
# ----------------------------------------------------------------------------


class Body:
    def __init__(self, gltf, sk, morph_weights=None):
        doc = gltf.doc
        node = next(i for i, n in enumerate(doc['nodes']) if 'mesh' in n and 'skin' in n)
        mesh = doc['meshes'][doc['nodes'][node]['mesh']]
        weights = morph_weights if morph_weights is not None else mesh.get('weights', [])
        ibm = gltf.read(doc['skins'][0]['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
        self.ibm = ibm.astype(float)
        self.joint_nodes = sk.joints
        pos, joints, wts, mats = [], [], [], []
        for p in mesh['primitives']:
            v = gltf.read(p['attributes']['POSITION']).astype(float)
            for w, t in zip(weights, p.get('targets', [])):
                if w:
                    v += w * gltf.read(t['POSITION'])
            pos.append(v)
            joints.append(gltf.read(p['attributes']['JOINTS_0']).astype(int))
            wts.append(gltf.read(p['attributes']['WEIGHTS_0']).astype(float))
            mats.append(np.full(len(v), p['material']))
        self.positions = np.concatenate(pos)
        self.joints = np.concatenate(joints)
        self.weights = np.concatenate(wts)
        self.material = np.concatenate(mats)
        self.material_names = [m['name'] for m in doc['materials']]
        self.dominant = np.array(sk.joints)[self.joints[np.arange(len(self.joints)), self.weights.argmax(1)]]

    def of_material(self, name):
        return np.flatnonzero(self.material == self.material_names.index(name))

    def dominated_by(self, sk, bone):
        return np.flatnonzero(self.dominant == sk.index[bone])

    def skin(self, pose, ids):
        mats = np.stack([pose.world(n) @ self.ibm[j] for j, n in enumerate(self.joint_nodes)])
        v = np.c_[self.positions[ids], np.ones(len(ids))]
        out = np.zeros((len(ids), 3))
        for k in range(4):
            m = mats[self.joints[ids, k]]
            out += self.weights[ids, k, None] * np.einsum('nij,nj->ni', m, v)[:, :3]
        return out


# ----------------------------------------------------------------------------
# Solving: damped Gauss-Newton on a residual, and the legs
# ----------------------------------------------------------------------------


def solve(residual, x, iterations=40, step_limit=8.0, tolerance=1e-4):
    """Minimise |residual(x)|^2 from x (degrees), the numerical Jacobian, a step capped to step_limit."""
    x = np.asarray(x, float).copy()
    r = residual(x)
    for _ in range(iterations):
        jac = np.empty((len(r), len(x)))
        for j in range(len(x)):
            e = np.zeros(len(x))
            e[j] = 1e-3
            jac[:, j] = (residual(x + e) - residual(x - e)) / 2e-3
        step = np.linalg.solve(jac.T @ jac + 1e-6 * np.eye(len(x)), jac.T @ r)
        biggest = np.abs(step).max()
        if biggest > step_limit:
            step *= step_limit / biggest
        trial = x - step
        rt = residual(trial)
        if rt @ rt > r @ r:  # back off rather than overshoot
            trial = x - 0.25 * step
            rt = residual(trial)
        x, r = trial, rt
        if biggest < tolerance:
            break
    residual(x)  # leave the caller's pose at the answer
    return x, r


def bone_pitch(pose, bone):
    """A bone's world pitch against its rest (degrees, + its far end toward the tail, as a leg swings
    back), about the dog's left: the change of its axis's angle in the side view."""
    i = pose.sk.index[bone]
    d0 = pose.sk.rest_wr[i][:, 1]
    d = rotation(pose.world(i))[:, 1]
    a = math.degrees(math.atan2(d[2], d[1]) - math.atan2(d0[2], d0[1]))
    return (a + 180.0) % 360.0 - 180.0


CHAINS = {'front': ('upperarm', 'forearm', 'wrist', 'paw_front'), 'rear': ('thigh', 'shin', 'hock', 'paw_rear')}
W_LOW = 0.4  # residual per degree the pastern / metatarsus strays from its wanted pitch (mm per degree)
W_ABDUCT = 0.3  # and per degree of sideways swing at the shoulder or hip


def solve_leg(dog, pose, side, kind, target, paw_rot, low_pitch=0.0, extra=()):
    """Bend one leg so its paw bone's head lands on `target` (world) with world rotation `paw_rot`:
    the upper and middle bones pitch freely, the lower one (pastern, metatarsus) leans toward
    `low_pitch`, and a little sideways swing at the top absorbs a body lean. `extra` adds
    (bone, world axis, value, weight) height or reach targets. Warm-started from the last key."""
    upper, mid, low, paw = (f'{b}_{side}' for b in CHAINS[kind])
    pose.orient(paw, paw_rot)
    chain = [pose.sk.index[n] for n in (upper, mid, low, paw)]
    base = {n: pose.q.get(n, IDENTITY) for n in (upper, mid, low)}
    ax, ay = gl(AX_X), gl(AX_Y)

    def apply(x):
        pose.q[upper] = qmul(base[upper], qmul(qaxis(ax, x[0]), qaxis(ay, x[3])))
        pose.q[mid] = qmul(base[mid], qaxis(ax, x[1]))
        pose.q[low] = qmul(base[low], qaxis(ax, x[2]))
        for i in chain:
            pose._cache.pop(i, None)

    def residual(x):
        apply(x)
        r = list((pose.world(paw)[:3, 3] - target) * 1000.0)
        r += [(bone_pitch(pose, low) - low_pitch) * W_LOW, x[3] * W_ABDUCT, 0.02 * x[0], 0.02 * x[1]]
        r += [(pose.world(b)[a, 3] - v) * 1000.0 * w for b, a, v, w in extra]
        return np.array(r)

    key = (side, kind)
    x, r = solve(residual, dog.ik.get(key, np.zeros(4)))
    dog.ik[key] = x
    miss = float(np.linalg.norm(r[:3])) / 1000.0
    dog.worst_reach = max(dog.worst_reach, miss)
    if dog.miss_log is not None:
        dog.miss_log.append((side, kind, miss, (pose.world(paw)[:3, 3] - target).round(4).tolist()))
    return miss


def plant(dog, pose, side, kind, travel=0.0, ahead=0.0, lift=0.0, pitch=0.0, low_pitch=0.0, extra=()):
    """A paw at its rest world transform, moved `ahead` (+ toward the head) and up `lift`, its sole
    pitched by `pitch` (degrees, + toes curling under), with the root's travel added."""
    paw = dog.sk.index[f'{CHAINS[kind][3]}_{side}']
    rest = dog.sk.rest_world[paw]
    target = rest[:3, 3] + np.array([0.0, lift, travel + ahead])
    r = qmat(qaxis(gl(AX_X), pitch)) @ rotation(rest)
    return solve_leg(dog, pose, side, kind, target, r, low_pitch, extra)


def plant_all(dog, pose, **kw):
    for side in ('L', 'R'):
        for kind in ('front', 'rear'):
            plant(dog, pose, side, kind, **kw)


# ----------------------------------------------------------------------------
# One breed's model
# ----------------------------------------------------------------------------

# A full blink in degrees of upper lid: build_dog.py's LID_CLOSE_UPPER (34 - -12) and LID_CLOSE_LOWER;
# refine_bernese_expression.py turns the Bernese upper lid's turn into its blink as angle / 41 degrees.
LID_UPPER, LID_LOWER = 46.0, 10.0
LID_FULL = {'golden': 46.0, 'bernese': 41.0}
GOLDEN_LEG = 0.294  # the golden's shoulder joint over its front paw bone (m): the gaits' scale

BALL_RADIUS = 0.033  # a tennis ball
STICK_RADIUS = 0.015  # a 3 cm stick
BALL_AHEAD = 0.30  # PickUp: the ball lies this far in front of the front paws' contact (m)
TOOTH_SQUEEZE = 0.002  # the ball may press this far into a tooth's envelope (a tennis ball's felt gives)


class Dog:
    """One breed's model and rig description, and what the clips solve on it."""

    def __init__(self, gltf_path, teeth=None):
        self.gltf = Gltf(gltf_path)
        self.sk = Skeleton(self.gltf)
        self.sk.carriage = carriage_from_rest_clip(self.gltf, self.sk)
        self.rig_path = self.gltf.path.with_name(self.gltf.path.stem + '.rig.json')
        self.rig = json.loads(self.rig_path.read_text(encoding='utf-8'))
        self.breed = self.rig.get('breed', 'golden')
        sk = self.sk
        existing = [a for a in self.gltf.doc['animations'] if a['name'] not in NAMES]
        first = existing[0]
        keyed = {c['target']['node'] for c in first['channels'] if c['target']['path'] != 'weights'}
        self.keyed = [j for j in sk.joints if j in keyed]
        for a in existing:
            if {c['target']['node'] for c in a['channels'] if c['target']['path'] != 'weights'} != keyed:
                raise ValueError(f'{self.gltf.path}: the existing clips key different joints')
        self.morph_channel = any(c['target']['path'] == 'weights' for c in first['channels'])
        lids = {sk.index[f'lid_{u}_{s}'] for u in ('upper', 'lower') for s in ('L', 'R')}
        if lids <= keyed:
            if self.breed not in LID_FULL:
                raise ValueError(f'{self.gltf.path}: no blink scale for a {self.breed} that keys its lid bones')
            self.lid_mode = 'bones'
        elif self.morph_channel and not lids & keyed:
            self.lid_mode = 'morph'
            self._samoyed_weights_setup(first)
        else:
            raise ValueError(f'{self.gltf.path}: the lids are neither keyed nor morphed')
        weights = self.rest_morph_weights() if self.morph_channel else None
        self.body = Body(self.gltf, sk, weights)
        rest = Pose(sk)
        self.pads = {}
        contacts = {}
        for side in ('L', 'R'):
            for kind in ('front', 'rear'):
                bone = f'{CHAINS[kind][3]}_{side}'
                ids = self.body.dominated_by(sk, bone)
                v = self.body.skin(rest, ids)
                self.pads[bone] = ids
                contacts[bone] = v[v[:, 1] < v[:, 1].min() + 0.002].mean(0)
        self.rest_contacts = contacts
        self.ground = float(np.mean([c[1] for c in contacts.values()]))
        self.front_contact_z = float(np.mean([contacts[f'paw_front_{s}'][2] for s in ('L', 'R')]))
        self.scale = (sk.head('upperarm_L')[1] - sk.head('paw_front_L')[1]) / GOLDEN_LEG
        self.ik = {}
        self.worst_reach = 0.0
        self.miss_log = None
        self._cache = {}
        self.baked = {}  # clip name -> its baked poses, for the clips that join it (clip_frame)
        self.teeth = (self.sk, self.body) if teeth is None else teeth_model(teeth)
        self.teeth_source = teeth

    # -- the Samoyed's morph channel --------------------------------------------------------------

    def _samoyed_weights_setup(self, clip):
        mesh = self.gltf.doc['meshes'][0]
        self.target_names = mesh['extras']['targetNames']
        need = [f'Blink_{s}_{k}' for s in ('L', 'R') for k in (1, 2, 3)] + ['MouthSeal']
        if any(n not in self.target_names for n in need):
            raise ValueError(f'{self.gltf.path}: a morph channel this script does not know: {self.target_names}')
        rest = next(a for a in self.gltf.doc['animations'] if a['name'] == 'Rest')
        ch = next(c for c in rest['channels'] if c['target']['path'] == 'weights')
        self.weights_node = ch['target']['node']
        self.rest_weights = self.gltf.read(rest['samplers'][ch['sampler']]['output']).reshape(
            -1, len(self.target_names))[0].astype(float)
        recipe = MODELS / 'Samoyed' / 'build_samoyed.py'
        spec = importlib.util.spec_from_file_location('samoyed_recipe', recipe)
        self.samoyed = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.samoyed)

    def rest_morph_weights(self):
        return list(self.gltf.doc['meshes'][0].get('weights', []))

    def morph_weights(self, blink, jaw_degrees):
        """One row of the Samoyed's weights channel: build_samoyed.py's blink basis and MouthSeal (the
        lips meet below 12 degrees of jaw), SamoyedJawThroat its complement as in every existing clip,
        the other sculpts as the Rest clip holds them."""
        w = self.rest_weights.copy()
        names = self.target_names
        basis = self.samoyed.basis_weights(np.array([blink]))[0]
        for s in ('L', 'R'):
            for k in (1, 2, 3):
                w[names.index(f'Blink_{s}_{k}')] = basis[k - 1]
        seal = 1.0 - float(self.samoyed.smoothstep(math.radians(jaw_degrees) / math.radians(12.0)))
        w[names.index('MouthSeal')] = seal
        if 'SamoyedJawThroat' in names:
            w[names.index('SamoyedJawThroat')] = 1.0 - seal
        return w

    # -- the mouth: the hold opening and the sockets ---------------------------------------------

    def mouth(self):
        """The jaw's HOLD opening and where a ball and a stick sit in it, solved on the teeth.

        The ball is centred on the midline half its radius behind the upper canines, at the height
        between the canine tips that leaves it the most room; HOLD is the smallest opening at which it
        clears every tooth but for TOOTH_SQUEEZE, so it touches both rows. The stick lies crosswise
        just behind the upper canines, on the lower canines' tips."""
        if 'mouth' in self._cache:
            return self._cache['mouth']
        sk, b = self.teeth
        teeth = b.of_material('DogTeeth')
        upper = teeth[b.dominant[teeth] == sk.index['head']]
        lower = teeth[b.dominant[teeth] == sk.index['jaw']]
        if len(upper) < 50 or len(lower) < 50:
            raise ValueError(f'{self.gltf.path}: cannot split the teeth into upper and lower rows')
        up = b.skin(Pose(sk), upper)

        def canine(points, low):
            """The longest tooth's tip within the front 45 mm of a row (the canine), off the midline."""
            pick = (points[:, 2] > points[:, 2].max() - 0.045) & (np.abs(points[:, 0]) > 0.014)
            pts = points[pick]
            return pts[np.argmin(pts[:, 1])] if low else pts[np.argmax(pts[:, 1])]

        upper_canine = canine(up, True)
        zc = upper_canine[2] - 0.5 * BALL_RADIUS

        def at(theta):
            p = Pose(sk)
            p.rot('jaw', AX_X, theta)
            lo = b.skin(p, lower)
            return p, lo, canine(lo, False)

        def room(theta):
            _p, lo, lower_canine = at(theta)
            pts = np.r_[up, lo]
            best = (-1.0, None)
            for y in np.linspace(lower_canine[1], upper_canine[1], 81):
                c = np.array([0.0, y, zc])
                clear = np.linalg.norm(pts - c, axis=1).min() - BALL_RADIUS
                if clear > best[0]:
                    best = (clear, c)
            return best

        hold = None
        for theta in np.arange(2.0, 45.0, 0.1):
            clear, c = room(theta)
            if clear >= -TOOTH_SQUEEZE:
                hold, ball, ball_clear = round(float(theta), 1), c, clear
                break
        if hold is None:
            raise ValueError(f'{self.gltf.path}: no jaw opening up to 45 degrees holds a {BALL_RADIUS * 2} m ball')
        p, lo, lower_canine = at(hold)
        inv = np.linalg.inv(p.world('jaw'))
        stick = np.array([0.0, lower_canine[1] + STICK_RADIUS, upper_canine[2] - STICK_RADIUS - 0.004])
        side = np.r_[up, lo][:, 1:] - stick[1:]  # a crosswise cylinder: the distance in the side view
        stick_clear = float(np.linalg.norm(side, axis=1).min() - STICK_RADIUS)
        if stick_clear < -TOOTH_SQUEEZE:
            raise ValueError(f'{self.gltf.path}: the stick sinks {-stick_clear * 1000:.1f} mm into a tooth')
        frame = matq(rotation(inv))

        def socket(c):
            local = (inv @ np.r_[c, 1.0])[:3]
            return dict(bone='jaw', translation=[round(float(v), 6) for v in local],
                        rotation=[round(float(v), 6) for v in frame])

        out = dict(hold=hold, ball_world=ball, ball_clear=float(ball_clear), stick_world=stick, stick_clear=stick_clear,
                   upper_canine=upper_canine, lower_canine=lower_canine,
                   sockets=dict(mouth=socket(ball), mouthStick=dict(socket(stick), longAxis=[1.0, 0.0, 0.0])))
        self._cache['mouth'] = out
        return out

    def socket_world(self, pose, which='mouth'):
        s = self.mouth()['sockets'][which]
        return (pose.world('jaw') @ np.r_[s['translation'], 1.0])[:3]

    # -- head carriage ----------------------------------------------------------------------------

    def carriage(self, pose, h):
        """h = (trunk, neck_01, neck_02, head, reach): pitches in degrees (+ nose down) and a reach in
        metres at the golden's size. The trunk tips about the hips (the pelvis), so the chest drops
        over forelegs that bend under it; the reach lowers the neck's root out of the shoulders and
        lengthens the neck a little, as a dog's neck does when it stretches down: the rigs' necks are
        short (0.26 to 0.35 m from neck_01 to the held ball, against 0.39 to 0.55 m of drop)."""
        trunk, n1, n2, hd, reach = h
        pose.rot('pelvis', AX_X, trunk)
        pose.rot('neck_01', AX_X, n1)
        pose.rot('neck_02', AX_X, n2)
        pose.rot('head', AX_X, hd)
        if reach:
            k = reach * self.scale
            pose.move('neck_01', (0.0, -0.5 * k, -k))
            for bone in ('neck_02', 'head'):
                i = self.sk.index[bone]
                along = self.sk.rest_world[i][:3, 3] - self.sk.rest_world[self.sk.parent[i]][:3, 3]
                along = along / np.linalg.norm(along)
                pose.move(bone, tuple(0.35 * k * np.array([along[0], -along[2], along[1]])))

    def muzzle_pitch(self, pose):
        i = self.sk.index['head']
        f = rotation(pose.world(i)) @ self.sk.rest_wr[i].T @ np.array([0.0, 0.0, 1.0])
        return math.degrees(math.atan2(-f[1], f[2]))

    def solve_head(self, target, muzzle, trunk, reach, jaw, start=(60.0, 30.0, -40.0), prior=None):
        """The neck and head pitches that put the held ball's centre on `target` (world) with the
        muzzle `muzzle` degrees below level, the trunk and the reach fixed. Returns (h, miss)."""

        def residual(x):
            p = Pose(self.sk)
            self.carriage(p, (trunk, x[0], x[1], x[2], reach))
            p.rot('jaw', AX_X, jaw)
            r = list((self.socket_world(p)[1:] - target[1:]) * 1000.0)
            r.append((self.muzzle_pitch(p) - muzzle) * 3.0)
            r.append((x[0] - 1.5 * x[1]) * 0.05)  # the lower neck bends more than the upper
            if prior is not None:
                r.extend((x - np.asarray(prior, float)) * 0.5)
            return np.array(r)

        x, r = solve(residual, np.array(start, float), iterations=60)
        return (trunk, *x, reach), float(np.linalg.norm(r[:2])) / 1000.0

    def head_lowest(self, h, jaw):
        """The lowest point of the head, jaw and tongue in carriage h, over the ground."""
        if 'head_ids' not in self._cache:
            bones = [self.sk.index[n] for n in ('head', 'jaw', 'tongue_01', 'tongue_02', 'tongue_03')]
            self._cache['head_ids'] = np.flatnonzero(np.isin(self.body.dominant, bones))
        p = Pose(self.sk)
        self.carriage(p, h)
        p.rot('jaw', AX_X, jaw)
        return float(self.body.skin(p, self._cache['head_ids'])[:, 1].min() - self.ground)

    def pickup_plan(self):
        """Where PickUp finds the ball and how the dog gets its mouth there.

        The ball lies BALL_AHEAD in front of the forepaws' contact, or, where the breed's neck cannot
        reach that far, as far as it reaches (the Samoyed's head sits back over its chest). At GRAB
        the held ball's centre is on the ball or up to GRAB_LIFT above it: an open jaw's chin hangs
        below the ball, and that lift keeps it within CHIN_PRESS of the ground. The trunk tips the
        least that reaches, the muzzle points down 40 to 60 degrees."""
        if 'pickup' in self._cache:
            return self._cache['pickup']
        hold = self.mouth()['hold']
        found = None
        start = (50.0, 25.0, -40.0)
        for ahead in np.arange(BALL_AHEAD, 0.059, -0.02):
            ball = np.array([0.0, self.ground + BALL_RADIUS, self.front_contact_z + ahead])
            top = ball + np.array([0.0, GRAB_LIFT, 0.0])
            h, miss = self.solve_head(top, 50.0, PICKUP_TRUNK_MAX, PICKUP_REACH, hold, start)
            if miss > 0.002:
                continue  # out of reach however far the trunk tips
            for trunk in np.arange(PICKUP_TRUNK_MIN, PICKUP_TRUNK_MAX + 0.1, 2.0):
                for lift in np.arange(0.0, GRAB_LIFT + 1e-6, 0.004):
                    for muzzle in (MUZZLE_AT_GRAB, MUZZLE_AT_GRAB - 10.0, MUZZLE_AT_GRAB + 10.0):
                        target = ball + np.array([0.0, lift, 0.0])
                        h, miss = self.solve_head(target, muzzle, float(trunk), PICKUP_REACH, hold, start)
                        natural = max(abs(a) for a in h[1:4]) < NECK_BEND_MAX
                        if miss < 0.002 and natural and self.head_lowest(h, hold) > -CHIN_PRESS:
                            found = (float(ahead), ball, float(trunk), float(lift), muzzle, h)
                            break
                    if found:
                        break
                if found:
                    break
            if found:
                break
        if found is None:
            raise ValueError(f'{self.gltf.path}: the mouth cannot reach a ball on the ground')
        ahead, ball, trunk, lift, muzzle, grab = found
        # Reaching: most of the way down along the same carriage, the mouth open above the ball.
        reach = tuple(0.8 * v for v in grab[:4]) + (grab[4],)
        plan = dict(ahead=ahead, ball=ball, trunk=trunk, lift=lift, muzzle=muzzle, grab=grab, reach=reach,
                    chin=self.head_lowest(grab, hold))
        self._cache['pickup'] = plan
        return plan

    def envelope(self, kind, drop):
        """How far along the ground (m, + ahead of its rest spot) a leg reaches with the body `drop`
        (golden size) lower than it stands: the stretch where its paw lands within 1.5 mm."""
        key = ('envelope', kind, drop)
        if key not in self._cache:
            saved = self.ik, self.worst_reach, self.miss_log
            self.miss_log = None
            reach = []
            for sign in (-1.0, 1.0):
                self.ik = {}
                u = 0.0
                while abs(u) < 0.3 * self.scale:
                    p = Pose(self.sk)
                    p.move('pelvis', (0.0, 0.0, -drop * self.scale))
                    if kind == 'front':
                        scapula(self, p, 'L', u + sign * 0.005 * self.scale)
                    if plant(self, p, 'L', kind, ahead=u + sign * 0.005 * self.scale) > 0.0015:
                        break
                    u += sign * 0.005 * self.scale
                reach.append(u)
            self.ik, self.worst_reach, self.miss_log = saved
            self._cache[key] = tuple(reach)
        return self._cache[key]

    def bow_reach(self):
        """How far ahead the bow's forepaws lie: BOW_REACH, or less where the breed's foreleg cannot
        lay its forearm down that far out at the bounce's highest and lowest (the Samoyed's)."""
        if 'bow' not in self._cache:
            saved = self.ik, self.worst_reach, self.miss_log
            self.miss_log = None
            reach = BOW_REACH * self.scale
            while reach > 0.02 * self.scale:
                worst = 0.0
                for t in (0.125, 0.375):
                    self.ik = {}
                    p = Pose(self.sk)
                    bow_body(self, p, t)
                    worst = max(worst, bow_foreleg(self, p, 'L', reach))
                if worst < 0.002:
                    break
                reach -= 0.005 * self.scale
            self.ik, self.worst_reach, self.miss_log = saved
            self._cache['bow'] = reach
        return self._cache['bow']

    def ball_on_ground(self):
        return self.pickup_plan()['ball']


# ----------------------------------------------------------------------------
# The golden's expression helpers (build_dog.py), on any breed
# ----------------------------------------------------------------------------


def blink(dog, r, amount):
    """Both eyes, `amount` of a full blink in the breed's own measure: lid bones about each eye's
    up x gaze axis (build_dog.py's blink), or for a morphed breed the amount its channel is given."""
    r.blink_amount = amount
    if dog.lid_mode != 'bones':
        return
    full = LID_FULL[dog.breed]
    for side in ('L', 'R'):
        eye = dog.rig['eyes'][side]
        axis = np.cross(np.asarray(eye['up'], float), np.asarray(eye['gaze'], float))
        r.rot_gl(f'lid_upper_{side}', axis, full * amount)
        r.rot_gl(f'lid_lower_{side}', axis, -full * LID_LOWER / LID_UPPER * amount)


def brows(r, raise_amount, worry=0.0):
    for s, side in ((1.0, 'L'), (-1.0, 'R')):
        r.move(f'brow_{side}', (s * -0.0012 * worry, -0.0015 * raise_amount, 0.0045 * raise_amount))
        r.rot(f'brow_{side}', AX_Y, s * 10.0 * worry)


def tail_wag(r, t, cycles, amplitude, lift=0.0, travel=0.55):
    for i in range(1, 7):
        w = 0.55 + 0.1 * i
        r.rot(f'tail_{i:02d}', AX_Z, amplitude * w * math.sin(2 * math.pi * cycles * t - travel * i))
        if lift:
            r.rot(f'tail_{i:02d}', AX_X, -lift * (0.3 + 0.12 * i))


def breathe(r, t, cycles, depth=1.0):
    s = math.sin(2 * math.pi * cycles * t)
    r.rot('spine_02', AX_X, 0.7 * depth * s)
    r.rot('spine_03', AX_X, -0.9 * depth * s)
    r.move('spine_03', (0.0, 0.0, 0.0018 * depth * s))


def ears(r, back, flap=0.0, phase=0.0, perk=0.0):
    """Both ears: swept back toward the tail (+, streaming), flapping, or perked forward at the base."""
    for side in ('L', 'R'):
        r.rot(f'ear_{side}_01', AX_X, back + flap * math.sin(phase) - perk)
        r.rot(f'ear_{side}_02', AX_X, 0.5 * back + 0.7 * flap * math.sin(phase - 0.8))
        r.rot(f'ear_{side}_03', AX_X, 0.25 * back + 0.5 * flap * math.sin(phase - 1.6))


def lerp(a, b, k):
    return tuple(x + (y - x) * k for x, y in zip(a, b))


# ----------------------------------------------------------------------------
# The clips. Distances are the golden's, times dog.scale; angles are every breed's.
# ----------------------------------------------------------------------------

RUN_FRAMES = 12  # one gallop stride, 0.4 s (2.5 strides a second, a medium dog's)
RUN_SPEED = 3.0  # m/s at the golden's size
RUN_DROP = 0.03  # m the body runs lower than it stands, which lengthens the legs' reach
RUN_FEET = {('L', 'rear'): 0.0, ('R', 'rear'): 0.08, ('R', 'front'): 0.40, ('L', 'front'): 0.48}
RUN_LIFT = {'front': 0.10, 'rear': 0.07}

CARRY_FRAMES = 14  # one trot stride, 0.467 s
CARRY_SPEED = 1.5
CARRY_DROP = 0.02
CARRY_FEET = {('L', 'rear'): 0.0, ('R', 'front'): 0.02, ('R', 'rear'): 0.5, ('L', 'front'): 0.52}
CARRY_LIFT = {'front': 0.06, 'rear': 0.05}
STANCE_SHARE = 0.8  # of the reach a leg has along the ground (Dog.envelope) a stance uses

PICKUP_FRAMES, GRAB = 36, 0.62
DROP_FRAMES, RELEASE = 30, 0.48
BOW_FRAMES = 48
PETTED_FRAMES = 60
CARRY_HEAD = (0.0, -14.0, -6.0, 14.0, 0.0)  # Dog.carriage: the head proud, the muzzle level
STAND = (0.0, 0.0, 0.0, 0.0, 0.0)
MUZZLE_AT_GRAB = 50.0  # degrees below level the muzzle points at GRAB, tried first (then 10 either way)
PICKUP_TRUNK_MIN, PICKUP_TRUNK_MAX = 14.0, 28.0  # degrees the trunk may tip to reach the ball
CHIN_PRESS = 0.008  # m the chin's skin may press into the ground as the jaw scoops the ball (the coat covers it)
NECK_BEND_MAX = 75.0  # degrees any of neck_01, neck_02 and head may bend at GRAB: past it a neck folds into the chest
GRAB_LIFT = 0.02  # m the held ball's centre may sit above the ball on the ground at GRAB
PICKUP_REACH = 0.05  # m (golden size): the neck's root lowers out of the shoulders, the neck stretches
OPEN_EXTRA = 12.0  # how much wider than HOLD the jaw opens before it closes on the toy


def _min_jerk(s):
    s = min(max(s, 0.0), 1.0)
    return s * s * s * (10.0 - 15.0 * s + 6.0 * s * s)


def gait(dog, r, t, feet, speed, seconds, lift, drop, swing_pastern, swing_hock):
    """Every paw on its stride: planted through its stance where it landed while the body travels
    over it at `speed`, then lifted and carried forward to its next landing (a minimum-jerk path
    along the body) and set down flat. Each leg's stance spans STANCE_SHARE of the ground its
    breed's leg reaches at this body height, centred in it (Dog.envelope); the stance's share of
    the stride (its duty factor) follows from the speed."""
    stride = speed * seconds  # the body travels one stride a cycle; so does each paw
    travel = stride * t
    r.move('root', (0.0, -travel, 0.0))
    for (side, kind), offset in feet.items():
        lo, hi = dog.envelope(kind, drop)
        stance = STANCE_SHARE * (hi - lo)
        centre = 0.5 * (lo + hi)
        duty = stance / stride
        ph = (t - offset) % 1.0
        if ph < duty:  # planted: the paw's world spot is fixed, so along the body it slides back
            ahead = centre + 0.5 * stance - stance * ph / duty
            up, pitch, low = 0.0, 0.0, 0.0
        else:
            s = (ph - duty) / (1.0 - duty)
            ahead = centre - 0.5 * stance + stance * _min_jerk(s)
            up = lift[kind] * dog.scale * math.sin(math.pi * min(1.0, s / 0.92)) ** 0.7
            flex = math.sin(math.pi * min(1.0, s / 0.85))
            if kind == 'front':
                pitch, low = 50.0 * flex, swing_pastern * flex
            else:
                pitch, low = 25.0 * flex, swing_hock * flex
        if kind == 'front':
            scapula(dog, r, side, ahead)
        plant(dog, r, side, kind, travel=travel, ahead=ahead, lift=up, pitch=pitch, low_pitch=low)


def scapula(dog, r, side, ahead):
    """The shoulder blade swings with the foreleg, a third of the leg's angle."""
    r.rot(f'scapula_{side}', AX_X, -0.35 * math.degrees(math.atan2(ahead, GOLDEN_LEG * dog.scale)))


def clip_run(dog, r, t):
    """A rotary gallop: the hind pair lands, then the fore pair, then the gathered flight. The back
    flexes into the gathered flight and stretches as the forelegs reach; the body bobs between the
    two flights; head up and forward, the ears and tail streaming, the jaw a little open."""
    seconds = RUN_FRAMES / FPS
    flex = math.cos(2 * math.pi * (t - 0.87))  # +1 gathered, -1 extended
    pitch = -3.5 * math.cos(2 * math.pi * (t - 0.12))  # rear down landing behind, nose down on the forelegs
    bob = 0.01 * dog.scale * math.cos(4 * math.pi * (t - 0.87)) - RUN_DROP * dog.scale
    r.move('pelvis', (0.0, 0.0, bob))
    r.rot('pelvis', AX_X, pitch - 9.0 * flex)
    for bone, k in (('spine_01', 0.7), ('spine_02', 0.6), ('spine_03', 0.5)):
        r.rot(bone, AX_X, 9.0 * flex * k)
    r.rot('neck_01', AX_X, 10.0 - 0.6 * pitch + 3.0 * flex)
    r.rot('neck_02', AX_X, -2.0)
    r.rot('head', AX_X, -8.0 - 0.4 * pitch - 0.8 * 9.0 * flex * 0.5)
    r.rot('jaw', AX_X, 9.0 + 3.0 * math.sin(2 * math.pi * t))
    ears(r, 32.0, flap=9.0, phase=4 * math.pi * t)
    brows(r, 0.2)
    tail_wag(r, t, 1, 7.0, lift=4.0)
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_X, 5.0 * flex * (0.4 + 0.1 * i))
    gait(dog, r, t, RUN_FEET, RUN_SPEED * dog.scale, seconds, RUN_LIFT, RUN_DROP,
         swing_pastern=95.0, swing_hock=40.0)


def clip_carry(dog, r, t):
    """A trot with the toy: the diagonal pairs a half stride apart, the head proud and steady, the
    jaw locked at the hold opening, the tail up and wagging."""
    seconds = CARRY_FRAMES / FPS
    bob = 0.007 * dog.scale * math.cos(4 * math.pi * (t - 0.46)) - CARRY_DROP * dog.scale
    r.move('pelvis', (0.0, 0.0, bob))
    r.rot('pelvis', AX_Y, 1.5 * math.sin(2 * math.pi * t))
    r.rot('spine_03', AX_Y, -1.5 * math.sin(2 * math.pi * t))
    dog.carriage(r, CARRY_HEAD)
    r.rot('neck_02', AX_X, -1.5 * math.cos(4 * math.pi * (t - 0.46)))  # the head rides the bob level
    r.rot('jaw', AX_X, dog.mouth()['hold'])
    ears(r, 10.0, flap=5.0, phase=4 * math.pi * t, perk=6.0)
    brows(r, 0.35)
    tail_wag(r, t, 1, 15.0, lift=14.0)
    gait(dog, r, t, CARRY_FEET, CARRY_SPEED * dog.scale, seconds, CARRY_LIFT, CARRY_DROP,
         swing_pastern=60.0, swing_hock=25.0)


def clip_pickup(dog, r, t):
    """1.2 s: from a stand the trunk tips and the neck reaches down, the jaw opening by 35%; the
    mouth comes down over the ball and closes on it at GRAB; the head rises to the carry."""
    hold = dog.mouth()['hold']
    plan = dog.pickup_plan()
    grab, reach = plan['grab'], plan['reach']
    if t < 0.52:
        h = lerp(STAND, reach, _ramp(t, 0.06, 0.52))
    elif t < GRAB:
        h = lerp(reach, grab, _ramp(t, 0.52, GRAB))
    else:
        h = lerp(grab, CARRY_HEAD, _ramp(t, 0.68, 1.0))
    dog.carriage(r, h)
    jaw = (hold + OPEN_EXTRA) * _ramp(t, 0.16, 0.35)
    jaw += (hold - (hold + OPEN_EXTRA)) * _ramp(t, 0.55, GRAB)
    r.rot('jaw', AX_X, jaw)
    ears(r, 6.0 * _ramp(t, 0.1, 0.5) + 6.0 * _ramp(t, 0.7, 1.0), perk=8.0 * _pulse(t, 0.08, 0.06, 0.1, 0.2))
    brows(r, 0.45 * _pulse(t, 0.1, 0.08, 0.3, 0.3) + 0.3 * _ramp(t, 0.7, 1.0))
    blink(dog, r, _pulse(t, 0.6, 0.04, 0.01, 0.05))
    tail_wag(r, t, 3, 8.0 + 6.0 * _ramp(t, 0.6, 1.0), lift=6.0 + 6.0 * _ramp(t, 0.6, 1.0))
    plant_all(dog, r)


def clip_drop(dog, r, t):
    """1 s: from the carry the head dips, the jaw lets go at RELEASE, the head comes back up to a
    stand and the tail wags, pleased."""
    hold = dog.mouth()['hold']
    dip = lerp(CARRY_HEAD, dog.pickup_plan()['grab'], 0.55)
    if t < 0.5:
        h = lerp(CARRY_HEAD, dip, _ramp(t, 0.06, 0.42))
    else:
        h = lerp(dip, STAND, _ramp(t, 0.52, 0.92))
    dog.carriage(r, h)
    jaw = hold + OPEN_EXTRA * _ramp(t, RELEASE - 0.04, RELEASE + 0.04)
    jaw *= 1.0 - _ramp(t, 0.62, 0.9)
    r.rot('jaw', AX_X, jaw)
    ears(r, 10.0 * (1.0 - _ramp(t, 0.5, 0.9)), perk=6.0 * _ramp(t, 0.55, 0.9))
    brows(r, 0.35 * (1.0 - _ramp(t, 0.4, 0.6)) + 0.4 * _pulse(t, 0.75, 0.1, 0.1, 0.1))
    blink(dog, r, _pulse(t, RELEASE + 0.02, 0.03, 0.01, 0.04))
    tail_wag(r, t, 4, 13.0 + 6.0 * _ramp(t, 0.45, 0.8), lift=10.0)
    plant_all(dog, r)


BOW_TRUNK = 26.0  # the pelvis tips the body nose down about the hips
BOW_REACH = 0.13  # the forepaws lie this far ahead of where they stood
BOW_ELBOW = 0.045  # the elbow's height over the ground


def clip_playbow(dog, r, t):
    """1.6 s loop, the play bow held with a bounce: elbows and forearms on the ground, the chest low,
    the rump up, the tail high and wagging fast, the head up looking ahead with the ears perked."""
    bow_body(dog, r, t)
    reach = dog.bow_reach()
    for side in ('L', 'R'):
        bow_foreleg(dog, r, side, reach)
        plant(dog, r, side, 'rear')


def bow_body(dog, r, t):
    bounce = math.sin(4 * math.pi * t)
    trunk = BOW_TRUNK + 2.5 * bounce
    r.move('pelvis', (0.0, 0.0, 0.02 * dog.scale))
    r.rot('pelvis', AX_X, trunk)
    r.rot('spine_02', AX_X, -4.0)
    r.rot('spine_03', AX_X, -3.0)
    r.rot('pelvis', AX_Y, 2.5 * math.sin(2 * math.pi * t))
    r.rot('neck_01', AX_X, -0.75 * trunk - 4.0)
    r.rot('neck_02', AX_X, -0.2 * trunk)
    r.rot('head', AX_X, -0.1 * trunk + 3.0 * math.sin(4 * math.pi * t - 0.9))
    r.rot('head', AX_Y, 4.0 * math.sin(2 * math.pi * t))
    r.rot('jaw', AX_X, 12.0 + 3.0 * math.sin(4 * math.pi * t))
    ears(r, -4.0, flap=4.0, phase=4 * math.pi * t, perk=10.0)
    brows(r, 0.6)
    blink(dog, r, _pulse(t, 0.7, 0.025, 0.01, 0.035))
    tail_wag(r, t, 6, 20.0, lift=10.0)


def bow_foreleg(dog, r, side, reach):
    """The forearm down along the ground (its elbow BOW_ELBOW up), the paw flat `reach` ahead."""
    elbow = dog.ground + BOW_ELBOW * dog.scale
    return plant(dog, r, side, 'front', ahead=reach, low_pitch=-70.0, extra=((f'forearm_{side}', 1, elbow, 0.6),))


def clip_petted(dog, r, t):
    """2 s loop, enjoying a hand: the head lowered and tilted into it, the neck leaning, the eyes half
    shut, slow deep breaths, the weight shifted toward the hand, the tail wagging fast."""
    s = math.sin(2 * math.pi * t)
    c = math.cos(2 * math.pi * t)
    breathe(r, t, 1, 2.0)
    r.move('pelvis', (0.01 * dog.scale * (1.0 + 0.2 * s), 0.0, -0.004 * dog.scale))
    r.rot('spine_01', AX_Y, 1.3 + 0.4 * s)
    r.rot('spine_02', AX_Y, 1.3 + 0.4 * s)
    r.rot('spine_01', AX_X, 1.5)  # settles the chest a little, so the forelegs keep some bend for the lean
    r.rot('pelvis', AX_Z, 2.0 * math.sin(2 * math.pi * 6 * t - 0.6))  # the rump wags with the tail
    r.rot('neck_01', AX_X, 12.0 + 2.0 * c)
    r.rot('neck_01', AX_Z, 10.0 + 2.0 * s)
    r.rot('neck_02', AX_Y, -8.0)
    r.rot('head', AX_X, 8.0 - 3.0 * c)
    r.rot('head', AX_Z, 8.0)
    r.rot('head', AX_Y, -22.0 - 5.0 * s)  # the tilt, leaning into the hand and back
    r.rot('jaw', AX_X, 3.0 + 1.0 * s)
    ears(r, 5.0 + 2.0 * s)  # relaxed, a little back: further and a drop ear flips up over the skull
    brows(r, -0.25)
    blink(dog, r, 0.5 + 0.05 * math.sin(2 * math.pi * t + 1.3))
    tail_wag(r, t, 6, 14.0, lift=3.0)
    plant_all(dog, r)


CLIPS = (('Run', RUN_FRAMES / FPS, clip_run, True), ('PickUp', PICKUP_FRAMES / FPS, clip_pickup, False),
         ('Carry', CARRY_FRAMES / FPS, clip_carry, True), ('Drop', DROP_FRAMES / FPS, clip_drop, False),
         ('PlayBow', BOW_FRAMES / FPS, clip_playbow, True), ('Petted', PETTED_FRAMES / FPS, clip_petted, True))
EVENTS = {'PickUp': {'grab': GRAB}, 'Drop': {'release': RELEASE}}


# ----------------------------------------------------------------------------
# The life clips: what the dog does between the games. Lying down and getting up, rolling over for a
# belly rub, trotting, sniffing, shaking off, stretching and yawning, scratching, nudging a hand,
# bouncing with excitement and turning on the spot. Transitions start and end EXACTLY on the frame
# they join (Idle's first, Sit's last, Lie's and BellyUp's first): Pose.start_from and
# Pose.blend_to put another clip's keys under or over the authored pose.
# ----------------------------------------------------------------------------

TORSO = ('pelvis', 'spine_01', 'spine_02', 'spine_03', 'neck_01', 'scapula_L', 'scapula_R')
LIE_BODY = TORSO + ('upperarm_L', 'upperarm_R', 'thigh_L', 'thigh_R')  # what a lying dog rests on
LIE_REST = LIE_BODY + ('forearm_L', 'forearm_R', 'shin_L', 'shin_R')
TAIL = tuple(f'tail_{i:02d}' for i in range(1, 7))
SETTLE_PRESS = 0.004  # m a body lying on the ground presses into it (the coat covers it)


def clip_frame(dog, name, at=0.0):
    """One keyed frame of a clip as joint locals (node -> t, q, s): a clip baked in this run, or one of
    the glTF's own, read at its key nearest phase `at` (0 its first key, 1 its last)."""
    key = ('frame', name, at)
    if key in dog._cache:
        return dog._cache[key]
    if name in dog.baked:
        poses = dog.baked[name]
        frame = poses[int(round(at * (len(poses) - 1)))].locals()
    else:
        anim = next(a for a in dog.gltf.doc['animations'] if a['name'] == name)
        parts = {}
        for ch in anim['channels']:
            node, path = ch['target']['node'], ch['target']['path']
            if path == 'weights':
                continue
            v = dog.gltf.read(anim['samplers'][ch['sampler']]['output'])
            parts.setdefault(node, {})[path] = v[int(round(at * (len(v) - 1)))].astype(float)
        sk = dog.sk
        frame = {n: (p.get('translation', sk.rest_t[n]), p.get('rotation', sk.rest_q[n]), p.get('scale', sk.rest_s[n]))
                 for n, p in parts.items()}
    dog._cache[key] = frame
    return frame


def part_ids(dog, bones, every=4):
    key = ('ids', tuple(bones), every)
    if key not in dog._cache:
        dog._cache[key] = np.flatnonzero(np.isin(dog.body.dominant, [dog.sk.index[b] for b in bones]))[::every]
    return dog._cache[key]


def lowest(dog, r, bones=TORSO, every=4):
    return float(dog.body.skin(r, part_ids(dog, bones, every))[:, 1].min())


def settle(dog, r, press=SETTLE_PRESS, bones=TORSO):
    """Move the whole body (the pelvis) so its lowest point of `bones` presses `press` into the ground."""
    r.move('pelvis', (0.0, 0.0, dog.ground - press - lowest(dog, r, bones)))


def place(dog, r, side, kind, offset=(0.0, 0.0, 0.0), pitch=0.0, low_pitch=0.0, extra=()):
    """A paw at its rest world transform moved by `offset` (glTF world: +x the dog's left, +y up, +z
    ahead), its sole pitched by `pitch` (+ toes curling under)."""
    paw = dog.sk.index[f'{CHAINS[kind][3]}_{side}']
    rest = dog.sk.rest_world[paw]
    target = rest[:3, 3] + np.asarray(offset, float)
    return solve_leg(dog, r, side, kind, target, qmat(qaxis(gl(AX_X), pitch)) @ rotation(rest), low_pitch, extra)


def joint_y(dog, bone):
    return float(dog.sk.rest_world[dog.sk.index[bone]][1, 3])


def chain_on_ground(dog, r, chain, axes=(AX_X,), press=SETTLE_PRESS):
    """Lift each bone of a chain (a tail, an ear) that sinks below the ground back onto it, root
    first, so it drapes along the grass: each turns the least, about whichever of `axes` lifts it
    best, that brings its skin up to the ground."""
    floor = dog.ground - press
    for i, bone in enumerate(chain):
        ids = part_ids(dog, chain[i:i + 2], 3)  # itself and the root of the next

        def low(axis, deg):
            p = r.copy()
            p.rot(bone, axis, deg)
            return float(dog.body.skin(p, ids)[:, 1].min())

        here = low(AX_X, 0.0)
        if here >= floor - 5e-4:
            continue
        # Of the turns that lift it (about each axis, either way), the least that brings it up to the
        # ground; but the way this bone turned at the key before while that still works, so a draped
        # chain does not flip from one way to the other between keys.
        best = None
        ways = [(axis, sign) for axis in axes for sign in (1.0, -1.0)]
        last = dog._cache.get(('drape', bone))
        if last in ways:
            ways.remove(last)
            ways.insert(0, last)
        for axis, sign in ways:
            hi = next((deg for deg in np.arange(5.0, 81.0, 5.0) if low(axis, sign * deg) >= floor), None)
            if hi is None:
                continue
            lo = hi - 5.0
            for _ in range(10):
                mid = 0.5 * (lo + hi)
                if low(axis, sign * mid) < floor:
                    lo = mid
                else:
                    hi = mid
            if best is None or hi < best[0]:
                best = (hi, axis, sign)
            if (axis, sign) == last:
                break
        if best is None:  # nothing lifts it all the way: lift it the most any turn does
            best = max(((deg, a, sg) for a in axes for sg in (1.0, -1.0) for deg in np.arange(5.0, 81.0, 5.0)),
                       key=lambda c: low(c[1], c[2] * c[0]))
            if low(best[1], best[2] * best[0]) <= here:
                continue
        dog._cache[('drape', bone)] = (best[1], best[2])
        r.rot(bone, best[1], best[2] * best[0])


def tail_on_ground(dog, r, press=SETTLE_PRESS, lift_root=False):
    """The tail draped on the grass where it would sink into it. With `lift_root` the whole tail first
    lifts at its root the least that keeps it out of the grass (up to 60 degrees), so a tail the body
    tips steeply into the ground (sitting) comes out of it as one piece rather than folding at every
    joint, which can fold one way at one key and another at the next."""
    if lift_root:
        ids = part_ids(dog, TAIL, 3)
        floor = dog.ground - press

        def low(deg):
            p = r.copy()
            p.rot('tail_01', AX_X, deg)
            return float(dog.body.skin(p, ids)[:, 1].min())

        if low(0.0) < floor:
            lo, hi = 0.0, 60.0
            if low(hi) >= floor:
                for _ in range(16):
                    mid = 0.5 * (lo + hi)
                    if low(mid) < floor:
                        lo = mid
                    else:
                        hi = mid
            r.rot('tail_01', AX_X, hi)
    chain_on_ground(dog, r, TAIL, (AX_X,), press)


def ears_on_ground(dog, r, press=SETTLE_PRESS):
    """The ears laid on the grass where they would sink into it (a dog on its back or side)."""
    for side in ('L', 'R'):
        chain_on_ground(dog, r, tuple(f'ear_{side}_{i:02d}' for i in (1, 2, 3)), (AX_X, AX_Y), press)


def pant_mouth(dog, r, at, jaw_extra=0.0):
    """The breed's own panting mouth at phase `at` of its Pant clip: the jaw, and the tongue out over
    the lower teeth as Pant lays it (the evidence holds Pant's tongue clear of every tooth)."""
    frame = clip_frame(dog, 'Pant', at)
    r.start_from(frame, ('jaw', 'tongue_01', 'tongue_02', 'tongue_03'))
    if jaw_extra:
        r.rot('jaw', AX_X, jaw_extra)


def wave(t, cycles, phase=0.0):
    return math.sin(2.0 * math.pi * cycles * t + phase)


# -- lying ---------------------------------------------------------------------------------------

LIE_PITCH = -7.0  # degrees the lying trunk tips nose-up: the chest proud on the elbows
LIE_SCAPULA = 14.0  # degrees the shoulder blades swing forward, the upper arms standing more upright
LIE_ELBOW = 0.045  # m (golden size) the elbows sit over the ground, the forearms flat
LIE_CARPUS = 0.048  # and the carpi, the pasterns flat
LIE_HOCK = 0.042  # and the hocks, the metatarsi flat
LIE_HOCK_BACK = 0.035  # m (golden size) the hock lies behind the hip: the hind leg folded tight under it
LIE_HIND_OUT = 0.035  # m (golden size) the folded hind leg lies outside where it stands


def two_bone(a, c, l1, l2, pole):
    """The middle joint of a two-bone chain from `a` reaching `c` (or toward it, straight, when out
    of reach), bent toward `pole`."""
    d = c - a
    dist = float(np.linalg.norm(d))
    u = d / dist
    dist = min(max(dist, abs(l1 - l2) + 1e-6), l1 + l2 - 1e-6)
    along = (l1 * l1 - l2 * l2 + dist * dist) / (2.0 * dist)
    v = np.asarray(pole, float) - np.dot(pole, u) * u
    v /= np.linalg.norm(v)
    return a + u * along + v * math.sqrt(max(0.0, l1 * l1 - along * along))


def aim(dog, r, bone, child, direction):
    """Turn `bone` (world) the least that points its joint-to-`child` vector along `direction`."""
    sk = dog.sk
    i, j = sk.index[bone], sk.index[child]
    d0 = sk.rest_world[j][:3, 3] - sk.rest_world[i][:3, 3]
    d0 = d0 / np.linalg.norm(d0)
    d = np.asarray(direction, float) / np.linalg.norm(direction)
    axis = np.cross(d0, d)
    s = float(np.linalg.norm(axis))
    angle = math.degrees(math.atan2(s, float(np.dot(d0, d))))
    turn = qmat(qaxis(axis / s, angle)) if s > 1e-9 else np.eye(3)
    r.orient(bone, turn @ sk.rest_wr[i])


def bone_length(dog, a, b):
    return float(np.linalg.norm(dog.sk.head(b) - dog.sk.head(a)))


def pitched(v, degrees):
    """A world direction turned about the dog's left-right axis (+ its far end toward the tail)."""
    return qmat(qaxis(gl(AX_X), degrees)) @ np.asarray(v, float)


def fold_leg(dog, r, side, kind, paw, low_dir, pitch=0.0, foot_rot=None, pole=None, pole_world=None):
    """A leg set outright (no solve): its paw bone's head at `paw` (world) with its sole pitched by
    `pitch`, the pastern / metatarsus pointing along `low_dir` into it, and the elbow / knee from
    the two upper bones' lengths, bending back (elbow) or forward and out (knee)."""
    upper, mid, low, foot = (f'{b}_{side}' for b in CHAINS[kind])
    top = r.world(upper)[:3, 3]
    low_dir = np.asarray(low_dir, float) / np.linalg.norm(low_dir)
    wrist = paw - low_dir * bone_length(dog, low, foot)
    sign = 1.0 if side == 'L' else -1.0
    if pole_world is not None:  # a world direction, e.g. where the knee was a key ago (no flips)
        pole = np.asarray(pole_world, float)
    elif pole is None:
        pole = np.array([0.0, -0.2, -1.0]) if kind == 'front' else np.array([sign * 0.35, -0.25, 1.0])
    else:
        pole = np.array([sign * pole[0], pole[1], pole[2]])
    elbow = two_bone(top, wrist, bone_length(dog, upper, mid), bone_length(dog, mid, low), pole)
    aim(dog, r, upper, mid, elbow - top)
    aim(dog, r, mid, low, wrist - elbow)
    aim(dog, r, low, foot, paw - wrist)
    rest = dog.sk.rest_world[dog.sk.index[foot]]
    r.orient(foot, qmat(qaxis(gl(AX_X), pitch)) @ (rotation(rest) if foot_rot is None else foot_rot))
    return float(np.linalg.norm(r.world(foot)[:3, 3] - paw))


def rest_low_dir(dog, side, kind):
    low, foot = (f'{b}_{side}' for b in CHAINS[kind][2:])
    d = dog.sk.head(foot) - dog.sk.head(low)
    return d / np.linalg.norm(d)


def lie_plan(dog):
    """Where the lying dog's body and paws go, set by its own bones: the pelvis drop that rests the
    belly on the ground at LIE_PITCH; each forearm flat on the ground from an elbow under its
    shoulder, the paw ahead of it; each hind leg folded with its hock behind the hip and its
    metatarsus flat. Then the body settles on what it rests on (the folded legs carry skin with them)."""
    if 'lie' in dog._cache:
        return dog._cache['lie']
    sk = dog.sk
    p = Pose(sk)
    p.rot('pelvis', AX_X, LIE_PITCH)
    drop = lowest(dog, p) - (dog.ground - SETTLE_PRESS)
    span = sk.head('spine_03')[2] - sk.head('pelvis')[2]
    plan = dict(hip=drop, shoulder=drop + span * math.sin(math.radians(LIE_PITCH)), span=span)
    dog._cache['lie'] = plan
    s = dog.scale
    flat = np.array([0.0, 0.0, 1.0])
    for _ in range(4):
        p = Pose(sk)
        lie_body(dog, p, 1.0, 1.0)
        for side in ('L', 'R'):
            top = p.world(f'upperarm_{side}')[:3, 3]
            lu = bone_length(dog, f'upperarm_{side}', f'forearm_{side}')
            ey = dog.ground + LIE_ELBOW * s
            ez = top[2] - math.sqrt(max(0.0, lu * lu - (top[1] - ey) ** 2))
            carpus = np.array([dog.sk.head(f'wrist_{side}')[0], dog.ground + LIE_CARPUS * s, 0.0])
            lf = bone_length(dog, f'forearm_{side}', f'wrist_{side}')
            carpus[2] = ez + math.sqrt(max(0.0, lf * lf - (ey - carpus[1]) ** 2))
            paw = carpus + flat * bone_length(dog, f'wrist_{side}', f'paw_front_{side}')
            paw[1] = joint_y(dog, f'paw_front_{side}')
            plan[('front', side)] = paw
            hip = p.world(f'thigh_{side}')[:3, 3]
            sign = 1.0 if side == 'L' else -1.0
            hock = np.array([hip[0] + sign * LIE_HIND_OUT * s, dog.ground + LIE_HOCK * s, hip[2] - LIE_HOCK_BACK * s])
            paw = hock + flat * bone_length(dog, f'hock_{side}', f'paw_rear_{side}')
            paw[1] = joint_y(dog, f'paw_rear_{side}')
            plan[('rear', side)] = paw
        p = Pose(sk)
        lying(dog, p, 1.0, 1.0, guard=False)
        error = lowest(dog, p, LIE_BODY) - (dog.ground - SETTLE_PRESS)
        plan['hip'] += error
        plan['shoulder'] += error
        if abs(error) < 2e-4:
            break
    return plan


def lie_body(dog, r, front, rear):
    """The trunk with the chest `front` and the hips `rear` of the way down to lying (0 standing, 1
    lying): the hips drop, the trunk tips about them to bring the chest down."""
    plan = dog._cache['lie'] if 'lie' in dog._cache else lie_plan(dog)
    down = rear * plan['hip']
    chest = front * plan['shoulder']
    r.move('pelvis', (0.0, 0.0, -down))
    tip = math.degrees(math.asin(max(-1.0, min(1.0, (chest - down) / plan['span']))))
    r.rot('pelvis', AX_X, tip)
    # The neck holds the head up against the trunk's tip (a dog keeps its eyes level going down).
    lean = tip - LIE_PITCH * min(front, rear)
    r.rot('neck_01', AX_X, -0.55 * lean)
    r.rot('neck_02', AX_X, -0.25 * lean)
    for side in ('L', 'R'):
        r.rot(f'scapula_{side}', AX_X, -LIE_SCAPULA * front)


def lie_leg(dog, r, side, kind, k, lift=0.0, pitch=0.0):
    """One leg `k` of the way from standing to lying, set outright: the paw moves from where it
    stands to where it lies (lifted `lift` on the way), the pastern or metatarsus turns from its
    standing slope to flat along the ground."""
    plan = dog._cache['lie']
    foot = f'{CHAINS[kind][3]}_{side}'
    stand = dog.sk.head(foot)
    paw = stand + (plan[(kind, side)] - stand) * k + np.array([0.0, lift, 0.0])
    low = rest_low_dir(dog, side, kind)
    flat = np.array([0.0, -0.06, 1.0]) / math.hypot(0.06, 1.0)
    direction = low + (flat - low) * k
    return fold_leg(dog, r, side, kind, paw, direction, pitch)


def lying(dog, r, front, rear, lift_front=0.0, lift_rear=0.0, guard=True, pitch_front=0.0):
    """The body and the four legs at these fold amounts. Going down the trunk tips over the elbows:
    with `guard`, nothing the dog rests on sinks into the ground (the body rides up on it)."""
    lie_body(dog, r, front, rear)
    for _ in range(3):
        for side in ('L', 'R'):
            lie_leg(dog, r, side, 'front', front, lift_front, pitch_front)
            lie_leg(dog, r, side, 'rear', rear, lift_rear)
        if not guard:
            return
        deficit = dog.ground - SETTLE_PRESS - lowest(dog, r, LIE_REST)
        if deficit <= 1e-4:
            return
        r.move('pelvis', (0.0, 0.0, deficit))


def lie_tail(dog, r, t, sweep=6.0, cycles=1.0, k=1.0):
    """The tail let down onto the grass behind the lying dog, curled round to one side."""
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_X, -(40.0 if i == 1 else 8.0) * k)
        r.rot(f'tail_{i:02d}', AX_Z, 7.0 * (0.4 + 0.12 * i) * k)
    tail_wag(r, t, cycles, sweep)


def lie_head(dog, r, t):
    """The lying dog's head up and looking about, slowly."""
    r.rot('neck_01', AX_X, -8.0 + 2.0 * wave(t, 1, 0.4))
    r.rot('neck_02', AX_X, 2.0)
    r.rot('head', AX_X, 6.0 + 2.0 * wave(t, 1, 1.5))
    r.rot('neck_02', AX_Z, 6.0 * wave(t, 1, 0.3))
    r.rot('head', AX_Z, 8.0 * wave(t, 1, 0.0))
    r.rot('head', AX_Y, 3.0 * wave(t, 1, 2.2))


LIE_FRAMES = 90  # 3 s loop


def clip_lie(dog, r, t):
    """3 s loop, lying sternal (the sphinx): the belly on the grass, the forearms flat ahead, the hind
    legs folded under. Slow deep breaths, the head up and looking about, a blink, an ear flick, the
    tail lying on the grass with an easy sweep."""
    lying(dog, r, 1.0, 1.0)
    breathe(r, t, 2, 1.6)
    lie_head(dog, r, t)
    tw = _pulse(t, 0.35, 0.03, 0.02, 0.07)
    r.rot('ear_R_01', AX_Y, 14.0 * tw)
    r.rot('ear_R_02', AX_Y, 7.0 * tw)
    ears(r, 4.0)
    brows(r, 0.15 * _pulse(t, 0.7, 0.1, 0.1, 0.15))
    blink(dog, r, _pulse(t, 0.58, 0.03, 0.015, 0.045))
    lie_tail(dog, r, t, sweep=7.0, cycles=1.0)
    tail_on_ground(dog, r)


LIEDOWN_FRAMES = 42  # 1.4 s


def clip_liedown(dog, r, t):
    """1.4 s, from a stand (Idle's first frame) to lying (Lie's first frame): the head dips to look
    where it lies, the forepaws walk ahead as the chest comes down onto the elbows (a bow), then the
    hind legs fold and the hips come down; the head rides up out of the drop and settles with a sigh."""
    front = _ease(_ramp(t, 0.06, 0.58))
    rear = _ease(_ramp(t, 0.26, 0.86))
    lift = 0.02 * dog.scale * math.sin(math.pi * _ramp(t, 0.08, 0.42))
    lying(dog, r, front, rear, lift_front=lift)
    look = _pulse(t, 0.3, 0.22, 0.05, 0.3)  # look down at the spot, then up as the hips land
    settle_bob = _pulse(t, 0.88, 0.05, 0.0, 0.1)
    r.rot('neck_01', AX_X, 14.0 * look - 8.0 * _ramp(t, 0.6, 0.95) + 3.0 * settle_bob)
    r.rot('head', AX_X, 10.0 * look + 6.0 * _ramp(t, 0.6, 0.95) - 2.0 * settle_bob)
    breathe(r, t, 1, -1.2 * _pulse(t, 0.9, 0.08, 0.0, 0.1))  # the sigh as it lands
    ears(r, 6.0 * _pulse(t, 0.5, 0.2, 0.1, 0.2), flap=5.0 * settle_bob, phase=math.pi * 8 * t)
    blink(dog, r, _pulse(t, 0.86, 0.04, 0.03, 0.06))
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_X, -(40.0 if i == 1 else 8.0) * _ease(_ramp(t, 0.4, 0.95)))
    tail_on_ground(dog, r)
    r.blend_to(clip_frame(dog, 'Idle', 0.0), 1.0 - _ramp(t, 0.0, 0.14))
    r.blend_to(clip_frame(dog, 'Lie', 0.0), _ramp(t, 0.84, 1.0))


GETUP_FRAMES = 66  # 1.1 s, keyed at 60 a second for the head shake


def clip_getup(dog, r, t):
    """1.1 s, from lying (Lie's first frame) to a stand (Idle's first frame): the hind legs push the
    hips up first (a bow), then the forelegs bring the chest up; a quick shake of the head, the ears
    flapping, and it stands."""
    rear = 1.0 - _ease(_ramp(t, 0.04, 0.46))
    front = 1.0 - _ease(_ramp(t, 0.2, 0.68))
    lift = 0.02 * dog.scale * math.sin(math.pi * _ramp(t, 0.36, 0.66))
    lying(dog, r, front, rear, lift_front=lift)
    push = _pulse(t, 0.22, 0.18, 0.05, 0.2)
    r.rot('neck_01', AX_X, -8.0 * (1.0 - _ramp(t, 0.3, 0.7)) + 6.0 * push)
    r.rot('head', AX_X, 6.0 * (1.0 - _ramp(t, 0.3, 0.7)) + 4.0 * push)
    shake = _pulse(t, 0.82, 0.06, 0.04, 0.08)
    r.rot('head', AX_Y, 20.0 * shake * math.sin(2 * math.pi * 7.0 * (t - 0.74) * 1.1))
    r.rot('neck_02', AX_Y, 8.0 * shake * math.sin(2 * math.pi * 7.0 * (t - 0.74) * 1.1 - 0.7))
    for s, side in ((1.0, 'L'), (-1.0, 'R')):
        r.rot(f'ear_{side}_01', AX_Y, s * 26.0 * shake * math.sin(2 * math.pi * 7.7 * (t - 0.74) - 1.2))
        r.rot(f'ear_{side}_02', AX_Y, s * 18.0 * shake * math.sin(2 * math.pi * 7.7 * (t - 0.74) - 2.0))
        r.rot(f'ear_{side}_03', AX_Y, s * 12.0 * shake * math.sin(2 * math.pi * 7.7 * (t - 0.74) - 2.8))
    blink(dog, r, max(0.6 * shake, _pulse(t, 0.12, 0.04, 0.02, 0.05)))
    k = 1.0 - _ease(_ramp(t, 0.02, 0.4))
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_X, -(40.0 if i == 1 else 8.0) * k)
    tail_wag(r, t, 3, 10.0 * _ramp(t, 0.4, 0.8))
    tail_on_ground(dog, r)
    r.blend_to(clip_frame(dog, 'Lie', 0.0), 1.0 - _ramp(t, 0.0, 0.1))
    r.blend_to(clip_frame(dog, 'Idle', 0.0), _ramp(t, 0.9, 1.0))


# -- on its back -----------------------------------------------------------------------------------

BELLY_ROLL = -148.0  # degrees the body rolls about its length: onto its right side and over onto its back
BELLY_FRAMES = 72  # 2.4 s loop
BELLY_NECK, BELLY_HEAD = 6.0, 4.0  # degrees: the head tipped back only as far as the grass lets it (head_down)
ROLLOVER_FRAMES = 30  # 1 s
ROLL_JAW = (0.0, 0.1)  # RollOver phases: the jaw opens (before the head turns on the neck: the teeth skin
#   partly to the neck, so a turn shifts them against a tongue that still lies on them),
ROLL_LIFT_UP, ROLL_SLIDE, ROLL_LIFT_DOWN = (0.06, 0.2), (0.2, 0.55), (0.55, 0.66)  # the tongue lifts, slides,
ROLL_HANG = (0.6, 0.9)  # settles, and its tip hangs down over the lower lip
ROLL_TONGUE_LIFT = 0.010  # m (golden size) the tongue lifts off the lower teeth to slide out over them


def body_bones(dog):
    """Every skinned bone but the tail's."""
    return tuple(dog.sk.names[j] for j in dog.sk.joints if not dog.sk.names[j].startswith('tail_'))


def head_down(dog, r, press=SETTLE_PRESS):
    """Let the head back down onto the grass (the lying-on-its-back dog's head rests on its cheek):
    turn the upper neck until the head's lowest point meets the ground."""
    ids = part_ids(dog, ('head', 'jaw', 'ear_L_01', 'ear_R_01', 'ear_L_02', 'ear_R_02'), 2)
    floor = dog.ground - press

    def low(deg):
        p = r.copy()
        p.rot('neck_02', AX_X, deg)
        return float(dog.body.skin(p, ids)[:, 1].min())

    if abs(low(0.0) - floor) < 1e-4:
        return
    sign = 1.0 if low(3.0) < low(-3.0) else -1.0  # the direction that lowers it
    lo, hi = -75.0, 75.0  # turns in that direction: negative ones raise the head
    for _ in range(16):
        mid = 0.5 * (lo + hi)
        if low(sign * mid) > floor:
            lo = mid
        else:
            hi = mid
    r.rot('neck_02', AX_X, sign * 0.5 * (lo + hi))


def belly_legs(r, t, loose=1.0):
    """The legs in the air, loose and a little bent: the forepaws curled at the wrists, the hind legs
    fallen open. They sway a little with the breathing and the tail's sweep, the paws trailing."""
    for s, side in ((1.0, 'L'), (-1.0, 'R')):
        k = 1.0 if side == 'L' else 0.85
        r.rot(f'upperarm_{side}', AX_X, -28.0 * k + 4.0 * loose * wave(t, 2, 0.3 * s))
        r.rot(f'upperarm_{side}', AX_Y, s * 10.0)
        r.rot(f'forearm_{side}', AX_X, -62.0 * k + 5.0 * loose * wave(t, 2, -0.5 + 0.3 * s))
        r.rot(f'wrist_{side}', AX_X, 70.0 + 8.0 * loose * wave(t, 2, -1.2 + 0.3 * s))
        r.rot(f'paw_front_{side}', AX_X, 35.0 + 6.0 * loose * wave(t, 2, -1.9 + 0.3 * s))
        r.rot(f'thigh_{side}', AX_X, -38.0 * k + 3.0 * loose * wave(t, 1, 0.8 * s))
        r.rot(f'thigh_{side}', AX_Y, -s * 24.0 + s * 3.0 * loose * wave(t, 1, 0.5))
        r.rot(f'shin_{side}', AX_X, 48.0 * k + 4.0 * loose * wave(t, 1, -0.4 + 0.8 * s))
        r.rot(f'hock_{side}', AX_X, -30.0 + 5.0 * loose * wave(t, 1, -1.0 + 0.8 * s))
        r.rot(f'paw_rear_{side}', AX_X, -15.0 + 6.0 * loose * wave(t, 1, -1.6 + 0.8 * s))


def clip_bellyup(dog, r, t):
    """2.4 s loop, on its back for a belly rub: rolled over onto its back (a little onto its right
    side, as a dog lies), the spine curved, the belly to the sky, the forepaws curled, the hind legs
    fallen open, the head tipped back onto the grass with the mouth open and happy, slow deep breaths,
    the tail sweeping the grass."""
    r.rot('pelvis', AX_Y, BELLY_ROLL)
    r.rot('spine_02', AX_Y, 10.0)  # the chest rolls back a little less than the hips: a twist
    for bone, k in (('spine_01', 1.0), ('spine_02', 1.0), ('spine_03', 0.8)):
        r.rot(bone, AX_Z, 5.0 * k + 1.5 * wave(t, 1, 0.6))  # the banana curve, swaying with the tail
    breathe(r, t, 2, 2.2)
    belly_legs(r, t)
    r.rot('neck_01', AX_X, BELLY_NECK)
    r.rot('neck_02', AX_Z, 14.0 + 2.0 * wave(t, 1, 1.0))
    r.rot('head', AX_X, BELLY_HEAD + 2.0 * wave(t, 2, 0.9))
    r.rot('head', AX_Y, -20.0 + 3.0 * wave(t, 1, 1.4))  # the face turned toward whoever rubs
    pant_mouth(dog, r, (2.0 * t) % 1.0)
    ears(r, -14.0, flap=3.0, phase=2 * math.pi * 2 * t)  # fallen open on the grass
    brows(r, 0.3)
    blink(dog, r, 0.3 + 0.7 * _pulse(t, 0.62, 0.03, 0.015, 0.05))
    tail_wag(r, t, 2, 22.0)
    settle(dog, r, bones=LIE_BODY)
    head_down(dog, r)
    ears_on_ground(dog, r)
    tail_on_ground(dog, r)


def clip_rollover(dog, r, t):
    """1 s, from lying (Lie's first frame) over onto its back (BellyUp's first frame): the head leads,
    turning and tipping back over its shoulder, the shoulders follow and the hips last; the legs tuck
    in as it goes over and fall open on the far side. The body keeps to the ground all the way."""
    a, b = clip_frame(dog, 'Lie', 0.0), clip_frame(dog, 'BellyUp', 0.0)
    sk = dog.sk
    # The mouth opens first, then the tongue lifts off the lower teeth, slides out over them, settles
    # onto them and only then hangs down (Pant's order): a tongue sliding or bending while it lies on
    # or behind the incisors goes through them.
    timing = {'jaw': ROLL_JAW, 'tongue_01': ROLL_SLIDE, 'tongue_02': ROLL_HANG, 'tongue_03': ROLL_HANG,
              'head': (0.08, 0.62), 'neck': (0.08, 0.68), 'spine_03': (0.08, 0.76), 'scapula': (0.08, 0.76),
              'upperarm': (0.1, 0.78), 'forearm': (0.12, 0.8), 'wrist': (0.14, 0.82), 'paw_front': (0.16, 0.84),
              'spine_02': (0.12, 0.82), 'spine_01': (0.16, 0.86), 'pelvis': (0.18, 0.88), 'thigh': (0.24, 0.92),
              'shin': (0.26, 0.94), 'hock': (0.28, 0.96), 'paw_rear': (0.3, 0.98), 'tail': (0.3, 1.0)}
    frame = {}
    for i, (ta, qa, sa) in a.items():
        name = sk.names[i]
        key = next((k for k in timing if name.startswith(k)), None)
        if key is None:
            key = 'head'  # the face, the jaw, the ears: with the head
        w = _ease(_ramp(t, *timing[key]))
        tb, qb, sb = b[i]
        frame[i] = (ta + (tb - ta) * w, slerp(qa, qb, w), sa + (sb - sa) * w)
    r.start_from(frame)
    lift = _ease(_ramp(t, *ROLL_LIFT_UP)) * (1.0 - _ease(_ramp(t, *ROLL_LIFT_DOWN)))
    r.move('tongue_01', (0.0, 0.0, ROLL_TONGUE_LIFT * dog.scale * lift))
    tuck = math.sin(math.pi * _ramp(t, 0.1, 0.9))
    for side in ('L', 'R'):
        r.rot(f'forearm_{side}', AX_X, -30.0 * tuck)
        r.rot(f'wrist_{side}', AX_X, 25.0 * tuck)
        r.rot(f'thigh_{side}', AX_X, -22.0 * tuck)
        r.rot(f'shin_{side}', AX_X, 25.0 * tuck)
    blink(dog, r, 0.3 * _ramp(t, 0.5, 1.0) + 0.6 * _pulse(t, 0.35, 0.08, 0.1, 0.1))
    settle(dog, r, bones=body_bones(dog))
    ears_on_ground(dog, r)
    tail_on_ground(dog, r)
    r.blend_to(a, 1.0 - _ramp(t, 0.0, 0.08))
    r.blend_to(b, _ramp(t, 0.92, 1.0))


# -- on the move -----------------------------------------------------------------------------------

TROT_FRAMES = 16  # one trot stride, 0.533 s
TROT_SPEED = 1.1  # m/s at the golden's size
TROT_DROP = 0.015
TROT_LIFT = {'front': 0.055, 'rear': 0.045}
TROT_HEAD = (0.0, -4.0, -2.0, 7.0, 0.0)  # Dog.carriage: the head easy, a little forward, the muzzle level

SNIFF_FRAMES = 60  # 2 s loop: two slow walking strides
SNIFF_SPEED = 0.25  # m/s at the golden's size
SNIFF_DUTY = 0.75  # a slow walk: three paws down most of the time
SNIFF_FEET = {('L', 'rear'): 0.0, ('L', 'front'): 0.25, ('R', 'rear'): 0.5, ('R', 'front'): 0.75}
SNIFF_LIFT = {'front': 0.035, 'rear': 0.03}
SNIFF_DROP = 0.02
SNIFF_NOSE = 0.035  # m (golden size) the nose's lowest point over the grass as it sweeps


def gait_strides(dog, r, t, feet, speed, seconds, lift, drop, swing_pastern, swing_hock, duty=None, strides=1):
    """gait() with `strides` strides a loop and, if given, the duty factor (a slow walk has more paws
    down than its legs' reach would give): each paw planted where it lands while the body travels
    over it, then carried forward to its next landing. The swing reaches the landing spot a tenth of
    the swing early and the paw comes straight down onto it: it does not skim the grass."""
    stride = speed * seconds / strides
    travel = speed * seconds * t
    r.move('root', (0.0, -travel, 0.0))
    for (side, kind), offset in feet.items():
        lo, hi = dog.envelope(kind, drop)
        stance = duty * stride if duty is not None else STANCE_SHARE * (hi - lo)
        share = stance / stride
        centre = 0.5 * (lo + hi)
        ph = (t * strides - offset) % 1.0
        if ph < share:
            ahead = centre + 0.5 * stance - stance * ph / share
            up, pitch, low = 0.0, 0.0, 0.0
        else:
            s = (ph - share) / (1.0 - share)
            ahead = centre - 0.5 * stance + stance * _min_jerk(s / 0.9)
            up = lift[kind] * dog.scale * math.sin(math.pi * s) ** 0.7
            flex = math.sin(math.pi * min(1.0, s / 0.85))
            if kind == 'front':
                pitch, low = 45.0 * flex, swing_pastern * flex
            else:
                pitch, low = 22.0 * flex, swing_hock * flex
        if kind == 'front':
            scapula(dog, r, side, ahead)
        plant(dog, r, side, kind, travel=travel, ahead=ahead, lift=up, pitch=pitch, low_pitch=low)


def clip_trot(dog, r, t):
    """A relaxed, happy trot: the diagonal pairs a half stride apart, the body rolling a little over
    each, the head easy and nodding against the bob, the ears bouncing a beat behind, the mouth soft
    and a little open, the tail up and wagging once a stride."""
    seconds = TROT_FRAMES / FPS
    bounce = math.cos(4 * math.pi * (t - 0.46))
    r.move('pelvis', (0.0, 0.0, 0.008 * dog.scale * bounce - TROT_DROP * dog.scale))
    r.rot('pelvis', AX_Y, 2.0 * wave(t, 1))
    r.rot('pelvis', AX_Z, 1.5 * wave(t, 1, 0.4))
    r.rot('spine_03', AX_Y, -2.0 * wave(t, 1, -0.3))
    r.rot('spine_02', AX_X, 0.8 * bounce)
    dog.carriage(r, TROT_HEAD)
    r.rot('neck_02', AX_X, -2.5 * math.cos(4 * math.pi * (t - 0.38)))  # the head nods against the bob
    r.rot('head', AX_X, 1.5 * math.cos(4 * math.pi * (t - 0.30)))
    r.rot('head', AX_Z, 2.0 * wave(t, 1, 1.0))
    r.rot('jaw', AX_X, 8.0 + 2.0 * math.cos(4 * math.pi * (t - 0.2)))
    ears(r, 6.0, flap=7.0, phase=4 * math.pi * t - 1.3, perk=4.0)
    brows(r, 0.3)
    tail_wag(r, t, 1, 16.0, lift=-6.0)
    gait_strides(dog, r, t, CARRY_FEET, TROT_SPEED * dog.scale, seconds, TROT_LIFT, TROT_DROP,
                 swing_pastern=55.0, swing_hock=22.0)


def periodic_path(t, points):
    """A loop through (phase, value) points (the first repeated at phase 1), eased between them, so
    the value rests at each point: a sweep with pauses in it."""
    pts = list(points) + [(1.0, points[0][1])]
    for (a, va), (b, vb) in zip(pts, pts[1:]):
        if a <= t <= b:
            return va + (vb - va) * _ease((t - a) / (b - a)) if b > a else vb
    return pts[0][1]


def nose_ids(dog):
    if 'nose' not in dog._cache:
        dog._cache['nose'] = dog.body.of_material('DogNose')[::3]
    return dog._cache['nose']


def sniff_carriage(dog):
    """The neck bend that brings the nose SNIFF_NOSE over the grass in the sniffing walk's carriage."""
    if 'sniff' in dog._cache:
        return dog._cache['sniff']

    def nose(n):
        p = Pose(dog.sk)
        p.move('pelvis', (0.0, 0.0, -SNIFF_DROP * dog.scale))
        dog.carriage(p, (6.0, n, 0.45 * n, -0.25 * n, 0.03))
        return float(dog.body.skin(p, nose_ids(dog))[:, 1].min()) - dog.ground - SNIFF_NOSE * dog.scale

    lo, hi = 0.0, 90.0
    for _ in range(20):
        mid = 0.5 * (lo + hi)
        if nose(mid) > 0.0:
            lo = mid
        else:
            hi = mid
    dog._cache['sniff'] = 0.5 * (lo + hi)
    return dog._cache['sniff']


def clip_sniff(dog, r, t):
    """2 s loop, a slow sniffing walk: three paws down most of the time, the nose low over the grass
    sweeping side to side and stopping to work a spot (quick small sniffs), the ears relaxed forward,
    the tail low-mid and wagging gently."""
    seconds = SNIFF_FRAMES / FPS
    n = sniff_carriage(dog)
    r.move('pelvis', (0.0, 0.0, -SNIFF_DROP * dog.scale + 0.003 * dog.scale * math.cos(4 * math.pi * t)))
    sweep = periodic_path(t, [(0.0, -0.8), (0.2, 0.85), (0.36, 0.85), (0.56, -0.15), (0.66, -0.15), (0.9, -0.8)])
    working = max(_pulse(t, 0.28, 0.06, 0.06, 0.04), _pulse(t, 0.61, 0.04, 0.03, 0.04))
    dip = 1.0 + 0.08 * working  # the nose goes in closer while it works a spot
    dog.carriage(r, (6.0, n * dip, 0.45 * n, -0.25 * n, 0.03))
    r.rot('neck_01', AX_Z, 16.0 * sweep)
    r.rot('head', AX_Z, 8.0 * sweep)
    r.rot('head', AX_Y, -5.0 * sweep)
    r.rot('head', AX_X, 2.5 * working * math.sin(2 * math.pi * 12 * t))  # sniff, sniff, sniff
    r.rot('pelvis', AX_Y, 1.2 * wave(t, 2))
    ears(r, -5.0 + 2.0 * working, flap=2.0, phase=4 * math.pi * t)
    brows(r, 0.2 + 0.3 * working)
    blink(dog, r, _pulse(t, 0.8, 0.025, 0.012, 0.035))
    tail_wag(r, t, 2, 9.0, lift=8.0)
    gait_strides(dog, r, t, SNIFF_FEET, SNIFF_SPEED * dog.scale, seconds, SNIFF_LIFT, SNIFF_DROP,
                 swing_pastern=45.0, swing_hock=18.0, duty=SNIFF_DUTY, strides=2)


# -- shaking off -----------------------------------------------------------------------------------

SHAKE_FRAMES = 66  # 1.1 s, keyed at 60 a second (4.5 shakes a second)
SHAKE_HZ = 4.5


def clip_shake(dog, r, t):
    """1.1 s, the wet-dog shake from a stand: it braces, then the roll starts at the head and runs
    back through the shoulders and the hips to the tail, each a beat behind the last, the ears
    flapping out past the head and the skin rolling with the spine; it dies away and it stands."""
    seconds = SHAKE_FRAMES / 60
    e = _ease(_ramp(t, 0.08, 0.22)) * (1.0 - _ease(_ramp(t, 0.62, 0.88)))
    brace = _pulse(t, 0.12, 0.1, 0.5, 0.2)
    ph = 2 * math.pi * SHAKE_HZ * seconds * (t - 0.08)
    r.move('pelvis', (0.0, 0.0, -0.012 * dog.scale * brace))
    # Each joint's roll adds to its parents': the shoulders roll about 10 degrees, the head 50.
    chain = (('head', 26.0, 0.0), ('neck_02', 12.0, 0.35), ('neck_01', 8.0, 0.7), ('spine_03', 4.5, 1.05),
             ('spine_02', 3.5, 1.4), ('spine_01', 3.0, 1.75), ('pelvis', 4.0, 2.1))
    for bone, amp, lag in chain:
        r.rot(bone, AX_Y, amp * e * math.sin(ph - lag))
    r.rot('head', AX_Z, 9.0 * e * math.sin(ph - 0.4))
    r.rot('neck_01', AX_X, 6.0 * brace)  # the head comes down a little into the shake
    r.rot('head', AX_X, 4.0 * brace)
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_Z, 12.0 * e * math.sin(ph - 2.4 - 0.4 * i))
        r.rot(f'tail_{i:02d}', AX_X, -4.0 * brace * (0.3 + 0.1 * i))
    for s, side in ((1.0, 'L'), (-1.0, 'R')):
        for k, (bone, amp) in enumerate(((f'ear_{side}_01', 34.0), (f'ear_{side}_02', 24.0), (f'ear_{side}_03', 16.0))):
            r.rot(bone, AX_Y, amp * e * math.sin(ph - 1.1 - 0.5 * k))
            r.rot(bone, AX_X, 10.0 * e * math.sin(2 * ph - 0.8 - 0.5 * k))
    r.rot('jaw', AX_X, 5.0 * e + 2.0 * e * math.sin(ph - 1.6))
    blink(dog, r, 0.75 * _ease(_ramp(t, 0.06, 0.18)) * (1.0 - _ease(_ramp(t, 0.72, 0.9))))
    brows(r, -0.2 * e)
    plant_all(dog, r)
    r.blend_to(clip_frame(dog, 'Idle', 0.0), 1.0 - _ramp(t, 0.0, 0.08))
    r.blend_to(clip_frame(dog, 'Idle', 0.0), _ramp(t, 0.9, 1.0))


# -- excited -----------------------------------------------------------------------------------------

EXCITED_FRAMES = 17  # 0.567 s loop, one bounce


def clip_excited(dog, r, t):
    """0.57 s loop, bouncing in place: it crouches on its forelegs, springs, and the forepaws leave
    the grass at the top (the hind paws stay down), lands and crouches again. The head up, the mouth
    open and panting, the ears perked and bouncing a beat behind, the tail wagging fast and wide."""
    up = math.cos(2 * math.pi * (t - 0.6))  # +1 at the top of the hop, -1 in the crouch
    air = _pulse(t, 0.6, 0.2, 0.04, 0.22)
    r.move('pelvis', (0.0, 0.0, (0.004 * up - 0.008) * dog.scale))
    r.rot('pelvis', AX_X, -4.5 - 8.0 * up)  # the chest low, then up
    r.rot('spine_03', AX_X, 2.0 * up)
    r.rot('pelvis', AX_Y, 2.0 * wave(t, 1, 0.6))
    r.rot('neck_01', AX_X, -6.0 + 6.5 * up)  # the head stays up as the chest rises and falls
    r.rot('neck_02', AX_X, -2.0 + 2.0 * math.cos(2 * math.pi * (t - 0.75)))
    r.rot('head', AX_X, -4.0 + 3.0 * math.cos(2 * math.pi * (t - 0.85)))
    r.rot('head', AX_Y, 6.0 * wave(t, 1, 0.2))
    pant_mouth(dog, r, 0.25 * t)
    ears(r, -4.0, flap=11.0, phase=2 * math.pi * t - 2.2, perk=9.0)
    brows(r, 0.7)
    tail_wag(r, t, 2, 30.0, lift=-8.0)
    for side in ('L', 'R'):
        lag = 0.03 if side == 'L' else 0.0
        a = _pulse(t - lag, 0.6, 0.2, 0.04, 0.22)
        scapula(dog, r, side, 0.02 * dog.scale * air)
        # The forelegs set outright (a solve can flip the elbow at the top of the hop): the paw up and
        # a little ahead, the carpus folding the paw back under.
        paw = dog.sk.head(f'paw_front_{side}') + np.array([0.0, 0.06 * a, 0.02 * a]) * dog.scale
        fold_leg(dog, r, side, 'front', paw, pitched(rest_low_dir(dog, side, 'front'), 70.0 * a), pitch=60.0 * a)
        plant(dog, r, side, 'rear')


# -- scratching ------------------------------------------------------------------------------------

SCRATCH_FRAMES = 108  # 1.8 s, keyed at 60 a second (six strokes a second)
SCRATCH_HZ = 6.0
SCRATCH_SEAT = ('pelvis', 'spine_01', 'thigh_R', 'shin_R', 'upperarm_L', 'upperarm_R', 'forearm_L', 'forearm_R')
SCRATCH_CURL = 2.0  # how far the body and neck curl round to the left, bringing the ear to the paw,
SCRATCH_DOWN = 0.9  # and how far down
SCRATCH_STRETCH = 1.0  # of its full length the scratching leg (hip to hock) stretches at most
SCRATCH_LEG_UP, SCRATCH_LEG_DOWN = (0.06, 0.32), (0.74, 0.96)  # phases the paw comes up and goes down in


def sit_paws(dog):
    """Where Sit's last frame puts each paw bone (world transform), and its lower bones' pitches."""
    if 'sitpaws' not in dog._cache:
        p = Pose(dog.sk)
        p.start_from(clip_frame(dog, 'Sit', 1.0))
        out = {}
        for side in ('L', 'R'):
            for kind in ('front', 'rear'):
                foot = f'{CHAINS[kind][3]}_{side}'
                low = p.world(foot)[:3, 3] - p.world(f'{CHAINS[kind][2]}_{side}')[:3, 3]
                out[(side, kind)] = (p.world(foot)[:3, 3].copy(), rotation(p.world(foot)).copy(),
                                     bone_pitch(p, f'{CHAINS[kind][2]}_{side}'), low / np.linalg.norm(low))
        dog._cache['sitpaws'] = out
    return dog._cache['sitpaws']


def clip_scratch(dog, r, t):
    """1.8 s, sitting (Sit's last frame): it leans onto its right hip, tips its head down and over to
    the left, brings the left hind paw up behind the left ear and scratches, six quick strokes a
    second, the eyes half shut and the ear flopping; then the paw goes back down and it sits."""
    seconds = SCRATCH_FRAMES / 60
    sit = clip_frame(dog, 'Sit', 1.0)
    r.start_from(sit)
    e = _ease(_ramp(t, 0.04, 0.24)) * (1.0 - _ease(_ramp(t, 0.8, 0.96)))
    leg = _ease(_ramp(t, *SCRATCH_LEG_UP)) * (1.0 - _ease(_ramp(t, *SCRATCH_LEG_DOWN)))
    strokes = _ramp(t, 0.24, 0.3) * (1.0 - _ramp(t, 0.72, 0.78))
    stroke = math.sin(2 * math.pi * SCRATCH_HZ * seconds * t)
    r.rot('pelvis', AX_Y, -10.0 * e)  # onto the right hip
    k, down = SCRATCH_CURL * e, SCRATCH_DOWN * e
    r.rot('spine_02', AX_Z, 9.0 * k)  # the body curls round to the left, toward the paw
    r.rot('spine_03', AX_Z, 9.0 * k)
    r.rot('spine_03', AX_X, 8.0 * down)
    r.rot('neck_01', AX_Z, 24.0 * k)  # the head comes round and a little down to the paw
    r.rot('neck_01', AX_X, 26.0 * down)
    r.rot('neck_02', AX_X, 8.0 * down)
    r.rot('head', AX_Y, 28.0 * e + 3.0 * strokes * math.sin(2 * math.pi * SCRATCH_HZ * seconds * t - 0.9))
    r.rot('head', AX_X, 6.0 * e)
    r.rot('jaw', AX_X, 7.0 * e)
    blink(dog, r, 0.55 * e)
    brows(r, 0.3 * e)
    r.rot('ear_L_01', AX_X, -12.0 * e + 9.0 * strokes * math.sin(2 * math.pi * SCRATCH_HZ * seconds * t - 1.4))
    r.rot('ear_L_02', AX_X, 7.0 * strokes * math.sin(2 * math.pi * SCRATCH_HZ * seconds * t - 2.1))
    paws = sit_paws(dog)
    # The paws it sits on stay where Sit put them, set outright; leaning onto the right hip must not
    # push it into the grass, so the body rides up on it where it would.
    def sitting_legs():
        for side, kind in (('L', 'front'), ('R', 'front'), ('R', 'rear')):
            pos, rot, _low, way = paws[(side, kind)]
            fold_leg(dog, r, side, kind, pos, way, foot_rot=rot, pole=SIT_KNEE if kind == 'rear' else None)

    sitting_legs()
    deficit = dog.ground - SETTLE_PRESS - lowest(dog, r, SCRATCH_SEAT)
    if deficit > 0.0:
        r.move('pelvis', (0.0, 0.0, deficit))
        sitting_legs()
    # The scratching paw: from where it sits to behind the left ear, then stroking down and back.
    ear = 0.5 * (r.world('ear_L_01')[:3, 3] + r.world('neck_02')[:3, 3])  # behind the ear, on the neck
    s = dog.scale
    rake = np.array([0.0, -0.025, -0.015]) * s * (0.5 + 0.5 * stroke) * strokes  # each stroke rakes down and back
    reach = ear + np.array([0.04, -0.02, -0.03]) * s + rake
    # Set outright, as the leg it sits on is: the paw on its way from the grass to the ear, the
    # metatarsus turning from flat to pointing up at it, the knee coming forward and out.
    pos, rot, _low, way = paws[('L', 'rear')]
    # On its way up and down the paw swings out and forward, around the hip rather than past it.
    target = pos + (reach - pos) * leg + np.array([0.04, 0.0, 0.08]) * s * math.sin(math.pi * leg)
    toes = qmat(qaxis(gl(AX_X), -70.0 * leg)) @ rot
    up = np.array([0.0, 0.8, 0.6])
    # Never quite straight: near full stretch the knee swings wide for every stroke.
    low_dir = way + (up - way) * leg
    low_dir = low_dir / np.linalg.norm(low_dir)
    hip = r.world('thigh_L')[:3, 3]
    hock = target - low_dir * bone_length(dog, 'hock_L', 'paw_rear_L')
    cap = SCRATCH_STRETCH * (bone_length(dog, 'thigh_L', 'shin_L') + bone_length(dog, 'shin_L', 'hock_L'))
    if np.linalg.norm(hock - hip) > cap:
        target = target - (hock - hip) / np.linalg.norm(hock - hip) * (np.linalg.norm(hock - hip) - cap)
    # The knee bends toward where it was a key ago (it starts as Sit's): as the hock passes the hip's
    # height a fixed bend direction lines up with the leg and the knee would flip across.
    if t == 0.0 or 'scratch_knee' not in dog._cache:
        miss = fold_leg(dog, r, 'L', 'rear', target, low_dir, foot_rot=toes, pole=SIT_KNEE)
    else:
        miss = fold_leg(dog, r, 'L', 'rear', target, low_dir, foot_rot=toes,
                        pole_world=dog._cache['scratch_knee'])
    dog._cache['scratch_knee'] = r.world('shin_L')[:3, 3] - r.world('thigh_L')[:3, 3]
    dog.worst_reach = max(dog.worst_reach, miss)
    tail_wag(r, t, 2, 5.0 * e)
    tail_on_ground(dog, r, lift_root=True)
    r.blend_to(sit, 1.0 - _ramp(t, 0.0, 0.05))
    r.blend_to(sit, _ramp(t, 0.95, 1.0))


# -- stretching and yawning ------------------------------------------------------------------------

STRETCH_FRAMES = 66  # 2.2 s
YAWN_JAW = 36.0  # degrees the jaw opens at the yawn's widest


def clip_stretch(dog, r, t):
    """2.2 s from a stand: the play-stretch. The forepaws walk out and the chest goes down to the
    elbows, the rump up (a slow bow) with a big yawn, the jaw wide and the tongue curling, the eyes
    shut and the ears back; then the weight shifts forward over the forelegs, the hips drop and each
    hind leg in turn stretches out behind, toes trailing; it steps back up into a stand."""
    s = dog.scale
    bow = _ease(_ramp(t, 0.04, 0.24)) * (1.0 - _ease(_ramp(t, 0.42, 0.56)))
    fwd = _ease(_ramp(t, 0.44, 0.58)) * (1.0 - _ease(_ramp(t, 0.82, 0.95)))
    reach = dog.bow_reach() * _ease(_ramp(t, 0.02, 0.2)) * (1.0 - _ease(_ramp(t, 0.84, 0.96)))
    shift = 0.07 * s * fwd
    r.move('pelvis', (0.0, -shift, 0.02 * s * bow - 0.03 * s * fwd))
    r.rot('pelvis', AX_X, (BOW_TRUNK + 2.0) * bow - 6.0 * fwd)
    r.rot('spine_02', AX_X, -5.0 * bow + 3.0 * fwd)
    r.rot('spine_03', AX_X, -4.0 * bow + 2.0 * fwd)
    yawn = _pulse(t, 0.27, 0.12, 0.05, 0.12)
    r.rot('neck_01', AX_X, -0.75 * (BOW_TRUNK + 2.0) * bow - 6.0 * yawn - 4.0 * fwd)
    r.rot('neck_02', AX_X, -0.2 * BOW_TRUNK * bow - 4.0 * fwd)
    r.rot('head', AX_X, -12.0 * yawn + 4.0 * fwd)
    r.rot('head', AX_Y, 5.0 * yawn)
    r.rot('jaw', AX_X, YAWN_JAW * yawn)
    curl = _pulse(t, 0.28, 0.08, 0.04, 0.1)
    r.rot('tongue_02', AX_X, -10.0 * curl)
    r.rot('tongue_03', AX_X, -38.0 * curl)  # the tip curls up
    ears(r, 18.0 * yawn + 6.0 * fwd)
    brows(r, -0.4 * yawn)
    blink(dog, r, max(0.95 * _pulse(t, 0.27, 0.1, 0.07, 0.1), 0.5 * fwd))
    tail_wag(r, t, 2, 6.0, lift=-10.0 * bow + 6.0 * fwd)
    flat = np.array([0.0, -0.06, 1.0]) / math.hypot(0.06, 1.0)
    for side in ('L', 'R'):  # set outright: the pasterns lie down flat as the chest goes down
        back = _pulse(t, 0.9, 0.05, 0.0, 0.05)  # the step back to where they stood
        paw = dog.sk.head(f'paw_front_{side}') + np.array([0.0, 0.03 * s * back, reach])
        low = rest_low_dir(dog, side, 'front')
        fold_leg(dog, r, side, 'front', paw, pitched(low + (flat - low) * 0.8 * bow, 30.0 * back), pitch=30.0 * back)
    for side, at in (('L', 0.62), ('R', 0.76)):
        ext = _pulse(t, at, 0.07, 0.03, 0.07)
        plant(dog, r, side, 'rear', ahead=-0.13 * s * ext, lift=0.035 * s * ext, pitch=-45.0 * ext,
              low_pitch=35.0 * ext)
    r.blend_to(clip_frame(dog, 'Idle', 0.0), 1.0 - _ramp(t, 0.0, 0.06))
    r.blend_to(clip_frame(dog, 'Idle', 0.0), _ramp(t, 0.93, 1.0))


# -- nudging a hand --------------------------------------------------------------------------------

NUDGE_FRAMES = 24  # 0.8 s
NUDGE_PEAK = 0.45
NUDGE_NECK = 5.0  # degrees the neck lifts at the push, and the muzzle tips up NUDGE_HEAD:
NUDGE_HEAD = 12.0  # with the neck's reach forward the nose rises 11 to 14 cm and comes 2 to 4 cm forward


def clip_nudge(dog, r, t):
    """0.8 s from a stand: a small dip, then the nose pushes up and forward into the hand above it,
    the neck stretching, the eyes half closing at the push; a little rub, and back."""
    s = dog.scale
    dip = _pulse(t, 0.14, 0.1, 0.0, 0.1)
    push = _ease(_ramp(t, 0.18, NUDGE_PEAK)) * (1.0 - _ease(_ramp(t, 0.66, 0.94)))
    over = 0.15 * _pulse(t, 0.8, 0.06, 0.0, 0.1)  # the head settles past the stand and back
    rub = _ramp(t, NUDGE_PEAK, 0.5) * (1.0 - _ramp(t, 0.6, 0.68))
    r.move('pelvis', (0.0, -0.015 * s * push, 0.0))  # the weight goes forward into it
    r.rot('neck_01', AX_X, 6.0 * dip - NUDGE_NECK * push + 6.0 * over)
    r.rot('neck_02', AX_X, -0.3 * NUDGE_NECK * push)
    r.rot('head', AX_X, 4.0 * dip - NUDGE_HEAD * push - 3.0 * over)
    r.move('neck_01', (0.0, -0.05 * s * push, 0.015 * s * push))
    r.rot('head', AX_Y, 6.0 * rub * math.sin(2 * math.pi * 3.0 * (t - NUDGE_PEAK)))
    r.rot('head', AX_Z, 3.0 * rub * math.sin(2 * math.pi * 3.0 * (t - NUDGE_PEAK) - 0.6))
    ears(r, 10.0 * push, flap=4.0 * dip, phase=8 * math.pi * t)
    brows(r, 0.4 * dip - 0.2 * push)
    blink(dog, r, 0.55 * _pulse(t, NUDGE_PEAK + 0.05, 0.12, 0.1, 0.16))
    tail_wag(r, t, 3, 10.0 + 8.0 * push, lift=-4.0)
    plant_all(dog, r)
    r.blend_to(clip_frame(dog, 'Idle', 0.0), 1.0 - _ramp(t, 0.0, 0.1))
    r.blend_to(clip_frame(dog, 'Idle', 0.0), _ramp(t, 0.9, 1.0))


# -- turning on the spot ---------------------------------------------------------------------------

TURN_FRAMES = 18  # 0.6 s loop
TURN_FEET = {('L', 'front'): 0.0, ('R', 'rear'): 0.25, ('R', 'front'): 0.5, ('L', 'rear'): 0.75}
TURN_DUTY = 0.62


def clip_turninplace(dog, r, t):
    """0.6 s loop, the footwork of turning on the spot (the script turns the root while it plays,
    either way): each paw in turn lifts, swings out a little and sets down where it stood, the body
    shifting its weight off the lifted paw. No travel, no turn of its own."""
    s = dog.scale
    sway = 0.0
    for (side, kind), offset in TURN_FEET.items():
        ph = (t - offset) % 1.0
        sign = 1.0 if side == 'L' else -1.0
        if ph < TURN_DUTY:
            lift = 0.0
        else:
            lift = math.sin(math.pi * (ph - TURN_DUTY) / (1.0 - TURN_DUTY))
        sway -= sign * lift * (1.0 if kind == 'front' else 0.7)
    r.move('pelvis', (0.006 * s * sway, 0.0, 0.0))  # the weight off the lifted paw
    r.rot('pelvis', AX_Y, -1.5 * sway)
    r.rot('spine_03', AX_Y, -0.8 * wave(t, 2, -0.2))
    r.move('pelvis', (0.0, 0.0, -0.004 * s + 0.003 * s * math.cos(4 * math.pi * t)))
    r.rot('neck_02', AX_X, 1.5 * math.cos(4 * math.pi * (t - 0.1)))
    ears(r, 3.0, flap=3.0, phase=4 * math.pi * t - 1.0)
    tail_wag(r, t, 1, 10.0, lift=2.0)
    for (side, kind), offset in TURN_FEET.items():
        ph = (t - offset) % 1.0
        sign = 1.0 if side == 'L' else -1.0
        lift = math.sin(math.pi * (ph - TURN_DUTY) / (1.0 - TURN_DUTY)) if ph >= TURN_DUTY else 0.0
        height = (0.035 if kind == 'front' else 0.03) * s * lift
        out = sign * 0.012 * s * lift
        if kind == 'front':
            place(dog, r, side, kind, (out, height, 0.008 * s * lift), pitch=40.0 * lift, low_pitch=45.0 * lift)
        else:
            place(dog, r, side, kind, (out, height, 0.0), pitch=20.0 * lift, low_pitch=-15.0 * lift)


# -- sitting: build_dog.py's Sit, its contacts solved ----------------------------------------------

# build_dog.py's clip_sit, as every breed's glTF carries it: the pelvis pitches nose-up and drops,
# the spine counters, the neck and head follow the chest. Its legs were angles: the rump stopped 53 to
# 103 mm over the grass while the folded hind paws and hocks went 11 to 30 mm into it, the forepaws
# slid back 62 to 82 mm and sank 15 mm (the golden) to 44 mm (the Samoyed, whose elbows sit off
# their bones' axes), and the Bernese's long tail went 110 mm into the ground. Here the same body
# motion sits on the grass: the hips come down until the rump rests on it, the hind paws stay where
# they stood while the hocks fold down flat behind them, the forepaws take one step back to stand
# under the chest, and the tail drapes on the grass.
SIT_FRAMES = 90  # 3 s, holding from 2.25 s
SIT_PELVIS_PITCH = 38.0  # build_dog.py's SIT_PELVIS_PITCH and SIT_SPINE_COUNTER
SIT_SPINE_COUNTER = 4.0
SIT_HIP_AHEAD = 0.0  # m (golden size) the seated hip joint lies this far ahead of the folded hock: over it
SIT_FORE_LEAD = 0.06  # m (golden size) the planted forepaw lies ahead of its shoulder
SIT_STEP_MAX = 0.10  # m (golden size): the step back is no longer; past it the forelegs lean forward more
SIT_KNEE = (0.4, 0.1, 1.0)  # the seated knee bends forward and a little out, beside the belly (x mirrored)
SIT_STEP_LIFT = 0.03  # m (golden size) the forepaws' step back clears the grass by


def sit_body(r, k):
    """The trunk, neck and head k of the way into build_dog.py's sit (the rotations only)."""
    r.rot('pelvis', AX_X, -SIT_PELVIS_PITCH * k)
    for bone in ('spine_01', 'spine_02', 'spine_03'):
        r.rot(bone, AX_X, SIT_SPINE_COUNTER * k)
    chest = SIT_PELVIS_PITCH - 3.0 * SIT_SPINE_COUNTER  # the chest's net nose-up pitch
    r.rot('neck_01', AX_X, 0.45 * chest * k)
    r.rot('neck_02', AX_X, 0.25 * chest * k)
    r.rot('head', AX_X, 0.15 * chest * k)


FLAT = np.array([0.0, -0.06, 1.0]) / math.hypot(0.06, 1.0)  # a pastern or metatarsus lying along the grass


def sit_hind(dog, r, side, k):
    """A hind paw planted where it stands, its metatarsus folding down flat behind it."""
    low = rest_low_dir(dog, side, 'rear')
    fold_leg(dog, r, side, 'rear', dog.sk.head(f'paw_rear_{side}'), low + (FLAT - low) * k, pole=SIT_KNEE)


def sit_plan(dog):
    """Where the seated hips go (back and down from the pelvis's rest, golden armature axes) so the
    hip lies SIT_HIP_AHEAD in front of the folded hock and the rump rests on the grass; and how far
    back each forepaw steps to stand SIT_FORE_LEAD ahead of its shoulder."""
    if 'sit' in dog._cache:
        return dog._cache['sit']
    s = dog.scale
    p = Pose(dog.sk)
    sit_body(p, 1.0)
    hip = p.world('thigh_L')[:3, 3]
    hock = dog.sk.head('paw_rear_L') - FLAT * bone_length(dog, 'hock_L', 'paw_rear_L')
    back = float(hip[2] - (hock[2] + SIT_HIP_AHEAD * s))
    drop = 0.0
    for _ in range(6):
        p = Pose(dog.sk)
        p.move('pelvis', (0.0, back, -drop))
        sit_body(p, 1.0)
        for side in ('L', 'R'):
            sit_hind(dog, p, side, 1.0)
        error = lowest(dog, p, LIE_BODY) - (dog.ground - SETTLE_PRESS)
        drop += error
        if abs(error) < 1e-5:
            break
    step = {}
    for side in ('L', 'R'):
        shoulder = p.world(f'upperarm_{side}')[:3, 3]
        want = float(dog.sk.head(f'paw_front_{side}')[2] - (shoulder[2] + SIT_FORE_LEAD * s))
        step[side] = min(max(0.0, want), SIT_STEP_MAX * s)
    plan = dict(back=back, drop=drop, step=step)
    dog._cache['sit'] = plan
    return plan


def clip_sit(dog, r, t):
    """3 s, holds: build_dog.py's sit with its contacts solved. The hips come back and down onto the
    grass with the trunk pitching up over them, the hind paws stay planted as the hocks fold down
    flat behind them, the forepaws take a step back under the chest, the tail curls round onto the
    grass; a breath, a blink, a slow wag."""
    plan = sit_plan(dog)
    s = dog.scale
    k = _ease(t / 0.75)
    breathe(r, t, 3, 0.8)
    r.move('pelvis', (0.0, plan['back'] * k, -plan['drop'] * k))
    sit_body(r, k)
    for i in range(1, 7):
        r.rot(f'tail_{i:02d}', AX_Z, 6.0 * k * (0.5 + 0.1 * i))
    brows(r, 0.3 * k)
    blink(dog, r, _pulse(t, 0.86, 0.02, 0.01, 0.03))
    tail_wag(r, t, 2, 4.0 * k)
    for side, lag in (('L', 0.0), ('R', 0.05)):
        a = _ease(_ramp(t, 0.1 + lag, 0.34 + lag))  # the step back, the right paw a beat behind
        up = math.sin(math.pi * a)
        paw = dog.sk.head(f'paw_front_{side}') + np.array([0.0, SIT_STEP_LIFT * s * up, -plan['step'][side] * a])
        fold_leg(dog, r, side, 'front', paw, pitched(rest_low_dir(dog, side, 'front'), 50.0 * up), pitch=40.0 * up)
        sit_hind(dog, r, side, k)
    tail_on_ground(dog, r, lift_root=True)  # the pelvis tips the tail's root into the grass


# The life clips follow the fetch clips, so every clip the glTF had keeps its index. Gaits and quick
# oscillations are keyed at 60 a second (KEY_RATE).
CLIPS = CLIPS + (('Trot', TROT_FRAMES / FPS, clip_trot, True), ('Sniff', SNIFF_FRAMES / FPS, clip_sniff, True),
                 ('Shake', SHAKE_FRAMES / 60, clip_shake, False), ('Lie', LIE_FRAMES / FPS, clip_lie, True),
                 ('LieDown', LIEDOWN_FRAMES / FPS, clip_liedown, False),
                 ('GetUp', GETUP_FRAMES / 60, clip_getup, False), ('BellyUp', BELLY_FRAMES / FPS, clip_bellyup, True),
                 ('RollOver', ROLLOVER_FRAMES / FPS, clip_rollover, False),
                 ('Excited', EXCITED_FRAMES / FPS, clip_excited, True),
                 ('Scratch', SCRATCH_FRAMES / 60, clip_scratch, False),
                 ('Stretch', STRETCH_FRAMES / FPS, clip_stretch, False),
                 ('Nudge', NUDGE_FRAMES / FPS, clip_nudge, False),
                 ('TurnInPlace', TURN_FRAMES / FPS, clip_turninplace, True))
EVENTS = dict(EVENTS, Nudge={'nudge': NUDGE_PEAK})
# The glTF's own clips this script re-authors where they stand (their index and rig entry kept),
# baked before the clips that start from them (Scratch from Sit's last frame).
REAUTHORED = (('Sit', SIT_FRAMES / FPS, clip_sit, False),)


# ----------------------------------------------------------------------------
# Baking and writing
# ----------------------------------------------------------------------------


# Keys a second per clip: FPS, as build_dog.py bakes, but the gallop's at 60. A stance there lasts
# two or three 30 fps keys and the leg sweeps 20 degrees between two of them, so a straight blend of
# its joint rotations sagged the planted paw 10 mm into the ground between keys and slid it 9 mm.
KEY_RATE = {'Run': 60, 'Trot': 60, 'GetUp': 60, 'Shake': 60, 'Scratch': 60}


def bake(dog, sampler, seconds, loop, rate=FPS):
    frames = int(round(seconds * rate))
    dog.ik = {}
    if loop:  # warm the leg solves on the stride's last third, so the first key is no special case
        for f in range(frames * 2 // 3, frames):
            sampler(dog, Pose(dog.sk), f / frames)
    poses = []
    for f in range(frames + 1):
        p = Pose(dog.sk)
        sampler(dog, p, f / frames)
        poses.append(p)
    return poses


def _keys(dog, poses):
    """Per keyed joint, per path, the values over the frames (rotations kept in one hemisphere)."""
    rows = [p.locals() for p in poses]
    out = {}
    for node in dog.keyed:
        for k, path in enumerate(('translation', 'rotation', 'scale')):
            v = np.array([row[node][k] for row in rows])
            if path == 'rotation':
                if v[0, 3] < 0:
                    v[0] = -v[0]
                for i in range(1, len(v)):
                    if np.dot(v[i], v[i - 1]) < 0:
                        v[i] = -v[i]
            out[(node, path)] = v
    return out


def write_animation(dog, name, seconds, poses):
    g = dog.gltf
    keys = _keys(dog, poses)
    times = np.linspace(0.0, seconds, len(poses))
    full = g.append(times, 'SCALAR', bounds=True)
    step = g.append([0.0, seconds], 'SCALAR', bounds=True)
    channels, samplers = [], []
    for (node, path), v in keys.items():
        kind = 'VEC4' if path == 'rotation' else 'VEC3'
        if np.abs(v - v[0]).max() < 1e-7:
            samplers.append(dict(input=step, interpolation='STEP', output=g.append([v[0], v[0]], kind)))
        else:
            samplers.append(dict(input=full, interpolation='LINEAR', output=g.append(v, kind)))
        channels.append(dict(sampler=len(samplers) - 1, target=dict(node=node, path=path)))
    if dog.morph_channel:
        # As build_samoyed.py samples it: 60 Hz over the clip, from the keys' lid amount and jaw angle.
        jaw = dog.sk.index['jaw']
        amounts = np.array([p.blink_amount for p in poses])
        angles = np.array([qangle(v, dog.sk.rest_q[jaw]) for v in keys[(jaw, 'rotation')]])
        wt = np.linspace(0.0, seconds, int(round(seconds * 60)) + 1)
        rows = [dog.morph_weights(float(np.interp(x, times, amounts)), float(np.interp(x, times, angles))) for x in wt]
        samplers.append(dict(input=g.append(wt, 'SCALAR', bounds=True), interpolation='LINEAR',
                             output=g.append(np.ravel(rows), 'SCALAR', bounds=True)))
        channels.append(dict(sampler=len(samplers) - 1, target=dict(node=dog.weights_node, path='weights')))
    animation = dict(channels=channels, name=name, samplers=samplers)
    slot = next((i for i, a in enumerate(g.doc['animations']) if a['name'] == name), None)
    if slot is None:
        g.doc['animations'].append(animation)
    else:  # a re-authored clip of the glTF's own keeps its index
        g.doc['animations'][slot] = animation


def rig_entries(dog):
    """What the rig description gains: the clips (with their event instants), the gaits' speeds, the
    toy sockets, and where PickUp expects the ball."""
    m = dog.mouth()
    ball = dog.ball_on_ground()
    clips = []
    for name, seconds, _f, loop in CLIPS:
        entry = dict(name=name, seconds=round(seconds, 6), loop=loop)
        if name in EVENTS:
            entry['events'] = EVENTS[name]
        clips.append(entry)
    return dict(clips=clips, runSpeed=round(RUN_SPEED * dog.scale, 6), carrySpeed=round(CARRY_SPEED * dog.scale, 6),
                trotSpeed=round(TROT_SPEED * dog.scale, 6), sniffSpeed=round(SNIFF_SPEED * dog.scale, 6),
                sockets=m['sockets'],
                fetch=dict(ballRadius=BALL_RADIUS, stickRadius=STICK_RADIUS, holdJawDegrees=round(m['hold'], 3),
                           pickUpBall=[round(float(v), 6) for v in ball]))


SPEEDS = ('runSpeed', 'carrySpeed', 'trotSpeed', 'sniffSpeed')


def merge_rig(rig, entries):
    """The existing rig description with these clips after its own (replacing earlier copies) and
    the new keys placed after walkSpeed (the gaits' root-motion speeds in the order SPEEDS lists)."""
    clips = [c for c in rig.get('clips', []) if c['name'] not in NAMES] + entries['clips']
    out = {}
    for k, v in rig.items():
        if k in SPEEDS or k in ('sockets', 'fetch'):
            continue
        out[k] = clips if k == 'clips' else v
        if k == 'walkSpeed':
            out.update((s, entries[s]) for s in SPEEDS)
    for s in SPEEDS:
        out.setdefault(s, entries[s])
    out['sockets'] = entries['sockets']
    out['fetch'] = entries['fetch']
    return out


def teeth_model(path):
    """The skeleton and body of a FINISHED model, for the mouth: the breed stages reshape the jaw and
    the teeth after the clips are authored (the Bernese's rounded jaw, the Samoyed's closed muzzle),
    so a hold solved on the editable source left the shipped ball 6 to 8 mm inside the teeth. Its
    morphs are weighted as its Pant clip opens the jaw (the closed-mouth sculpts off)."""
    g = Gltf(path)
    sk = Skeleton(g)
    weights = None
    pant = next((a for a in g.doc.get('animations', []) if a['name'] == 'Pant'), None)
    if pant is not None:
        ch = next((c for c in pant['channels'] if c['target']['path'] == 'weights'), None)
        if ch is not None:
            count = len(g.doc['meshes'][0]['weights'])
            weights = list(g.read(pant['samplers'][ch['sampler']]['output']).reshape(-1, count)[0].astype(float))
    return sk, Body(g, sk, weights)


def installed_model(gltf_path):
    """A breed source's finished model (Assets/Models/<breed>/<breed>.gltf), or None for a model that
    is itself finished (the golden, an installed breed)."""
    path = Path(gltf_path).resolve()
    installed = (MODELS / path.parent.name / path.name).resolve()
    return installed if installed != path and installed.is_file() else None


# The closed mouth. The Bernese's and the Samoyed's warps left the tongue too broad for the dental
# arch: at rest the Bernese's upper teeth stood 0.31 mm inside its tip and a lower tooth touched it,
# the Samoyed's teeth 0.35 mm clear, and a clip that moves the head or the jaw a little went further
# in (the Bernese's Shake 0.61 mm; DogShowcaseEvidenceTest's measure, on the finished model). The
# tongue's own joints are refitted, their rest and every clip's keys alike: tongue_01 narrowed (its x,
# which the whole tongue inherits) and pitched about its own x (+ tip down) or moved in its parent's
# (the jaw's) frame, tongue_03 (the tip) scaled down. Fitted on the finished models for the least
# clearance over jittered fits (Bernese) or over the jaw's first 14 degrees with the morphs its clips
# give them and the head pitched on the neck (Samoyed, whose MouthSeal opens with the jaw); the closed
# mouth then clears every tooth by 1.9 mm (Bernese) and 1.7 mm (Samoyed). The golden's, 1.3 mm, is
# its own. The bind geometry, the skin and the morphs are untouched: only the tongue's bones move.
TONGUE = ('tongue_01', 'tongue_02', 'tongue_03')
TONGUE_FIT = {
    'bernese': dict(offset=(0.0, 0.00008, 0.00005), pitch=-1.5,
                    scale={'tongue_01': (0.917, 1.0, 1.0), 'tongue_03': 0.918}),
    'samoyed': dict(offset=(0.0, 0.0, 0.0), pitch=0.0, scale={'tongue_01': (0.94, 1.0, 1.0), 'tongue_03': 0.93}),
}


def fit_tongue(g, breed):
    """Refit the tongue's joints (TONGUE_FIT) in a glTF: their rest, and their keys in every clip, each
    key moved by the change from the joint's current rest to the fitted one (so a clip that keys the
    rest gets the fitted rest, and Pant's tongue keeps its motion relative to it). The rest before the
    first fit is kept in the node's extras, so running again changes nothing. Returns whether anything
    changed."""
    fit = TONGUE_FIT.get(breed)
    if fit is None:
        return False
    doc = g.doc
    index = {n.get('name'): i for i, n in enumerate(doc['nodes'])}
    deltas = {}
    for bone in TONGUE:
        node = doc['nodes'][index[bone]]
        cur = {'translation': np.array(node.get('translation', [0.0] * 3), float),
               'rotation': np.array(node.get('rotation', [0.0, 0.0, 0.0, 1.0]), float),
               'scale': np.array(node.get('scale', [1.0] * 3), float)}
        orig = {k: np.array(v, float) for k, v in node.get('extras', {}).get('restBeforeTongueFit', cur).items()}
        want = dict(orig)
        want['scale'] = orig['scale'] * np.asarray(fit['scale'].get(bone, 1.0), float)
        if bone == 'tongue_01':
            want['translation'] = orig['translation'] + np.asarray(fit['offset'], float)
            if fit['pitch']:
                q = qmul(orig['rotation'], qaxis((1.0, 0.0, 0.0), fit['pitch']))
                want['rotation'] = q / np.linalg.norm(q)
        change = {}
        for path in ('translation', 'rotation', 'scale'):
            a, b = cur[path], want[path]
            gap = min(np.abs(a - b).max(), np.abs(a + b).max()) if path == 'rotation' else np.abs(a - b).max()
            if gap > 1e-9:
                change[path] = (b - a if path == 'translation' else qmul(qinv(a), b) if path == 'rotation' else b / a)
                node[path] = b.tolist()
        if change:
            node.setdefault('extras', {}).setdefault('restBeforeTongueFit', {k: v.tolist() for k, v in cur.items()})
            deltas[index[bone]] = change
    if not deltas:
        return False
    users = {}
    for a in doc['animations']:
        for ch in a['channels']:
            if ch['target']['path'] != 'weights':
                users.setdefault(a['samplers'][ch['sampler']]['output'], set()).add(
                    (ch['target']['node'], ch['target']['path']))
    done = {}
    for a in doc['animations']:
        for ch in a['channels']:
            node, path = ch['target']['node'], ch['target']['path']
            if node not in deltas or path not in deltas[node]:
                continue
            sampler = a['samplers'][ch['sampler']]
            out = sampler['output']
            if (out, node, path) in done:  # the same data under the same joint's channel in another clip
                sampler['output'] = done[(out, node, path)]
                continue
            delta = deltas[node][path]
            v = g.read(out).astype(float)
            if path == 'translation':
                v = v + delta
            elif path == 'rotation':
                v = np.array([qmul(k, delta) for k in v])
                v /= np.linalg.norm(v, axis=1, keepdims=True)
            else:
                v = v * delta
            if users[out] == {(node, path)}:  # its own data: rewrite it where it lies
                acc = doc['accessors'][out]
                view = doc['bufferViews'][acc['bufferView']]
                start = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
                raw = np.ascontiguousarray(v.astype('<f4'))
                g.data[start:start + raw.nbytes] = raw.tobytes()
                if 'min' in acc:
                    acc['min'], acc['max'] = raw.min(0).tolist(), raw.max(0).tolist()
                done[(out, node, path)] = out
            else:  # shared with another joint's channel: this channel gets its own copy
                done[(out, node, path)] = sampler['output'] = g.append(v, 'VEC4' if path == 'rotation' else 'VEC3')
    return True


def append_clips(gltf_path, write_rig=True, log=print, teeth=None):
    """Append (or replace) the play clips in `gltf_path` and its buffer; return the rig entries.
    With write_rig, <model>.rig.json beside it is updated too. The mouth is solved on `teeth` (a
    finished model's glTF), by default the installed model a breed source is rebuilt into."""
    teeth = teeth or installed_model(gltf_path)
    if teeth is not None:
        log(f'[clips] {Path(gltf_path).name}: the mouth solved on the finished model {teeth}')
    g = Gltf(gltf_path)
    rig_path = g.path.with_name(g.path.stem + '.rig.json')
    if fit_tongue(g, json.loads(rig_path.read_text(encoding='utf-8')).get('breed', 'golden')):
        g.write()
        log(f'[clips] {Path(gltf_path).name}: the tongue refitted to the closed mouth (TONGUE_FIT)')
    dog = Dog(gltf_path, teeth)
    dog.gltf.remove_animations(NAMES, [c[0] for c in REAUTHORED])
    for name, seconds, sampler, loop in REAUTHORED + CLIPS:
        dog.worst_reach = 0.0
        poses = bake(dog, sampler, seconds, loop, KEY_RATE.get(name, FPS))
        dog.baked[name] = poses
        write_animation(dog, name, seconds, poses)
        log(f'[clips] {dog.gltf.path.name} {name}: {seconds:.3f} s, {len(poses)} keys, '
            f'worst paw miss {dog.worst_reach * 1000:.1f} mm')
    dog.gltf.write()
    entries = rig_entries(dog)
    if write_rig:
        rig = merge_rig(dog.rig, entries)
        dog.rig_path.write_text(json.dumps(rig, indent=2) + '\n', encoding='utf-8')
    return entries


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('gltf', nargs='+', type=Path,
                        help='a dog model or breed source (<name>.gltf beside <name>.rig.json)')
    parser.add_argument('--teeth', type=Path,
                        help='solve the mouth on this finished model instead of the installed one')
    args = parser.parse_args()
    for path in args.gltf:
        append_clips(path, teeth=args.teeth)

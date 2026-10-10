"""Samoyed-only authoring from an explicit, frozen source model.

Usage: python build_samoyed.py --source SOURCE_DIRECTORY --output OUTPUT_DIRECTORY

The source contains Samoyed.gltf/.bin/.rig.json, maps, and Samoyed.abc. Output
never overwrites the source. It authors a corner-pinned blink, closed-mouth lip
contact, nasal shape, and plume carriage on that model. Rest topology and the
coat source remain intact. This is an editing recipe, not yet the from-scratch
surface/groom generator. A build manifest records inputs and geometric checks.
This is the earlier editing recipe. See README.md for the selected 2026-10-09
authored checkpoint; this script alone does not reconstruct its final refinements.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil

import numpy as np


# The frozen Samoyed's open aperture. These are authoring dimensions, in degrees.
UPPER_OPEN, LOWER_OPEN, MEET, CANTHUS = 26.0, -18.0, -12.0, -4.0
INNER_FISSURE, OUTER_FISSURE, OUTER_DROOP = 58.0, 64.0, -12.0
BLINK_STEPS = 3
LIP_TUCK = .0065
NOSE_HEIGHT = .80
TAIL_LAY = 60.0
TAIL_ROLL = 25.0
TAIL_BEND = 15.0
MUZZLE_SHORTENING = .014
MUZZLE_BREADTH = .09
# Eye midpoint of the source frame in which the facial fields were authored.
# Follow the rig when a source revision changes head carriage.
FACE_ANCHOR = np.array((0.0, .5816400053320032, .4248779033946991))
TYPES = {5121: 'u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class Gltf:
    def __init__(self, path):
        self.doc = json.loads(path.read_text())
        assert len(self.doc['buffers']) == 1
        self.data = bytearray((path.parent / self.doc['buffers'][0]['uri']).read_bytes())

    def read(self, index):
        entry = self.doc['accessors'][index]
        view = self.doc['bufferViews'][entry['bufferView']]
        dtype, width = np.dtype(TYPES[entry['componentType']]), WIDTHS[entry['type']]
        return np.ndarray((entry['count'], width), dtype=dtype, buffer=self.data,
                          offset=view.get('byteOffset', 0) + entry.get('byteOffset', 0),
                          strides=(view.get('byteStride', dtype.itemsize * width), dtype.itemsize)).copy()

    def append(self, values, kind, component=5126):
        values = np.asarray(values, dtype=TYPES[component]).reshape(-1, WIDTHS[kind])
        self.data.extend(b'\0' * (-len(self.data) % 4))
        view = len(self.doc['bufferViews'])
        self.doc['bufferViews'].append(dict(buffer=0, byteOffset=len(self.data), byteLength=values.nbytes))
        self.data.extend(values.tobytes())
        accessor = dict(bufferView=view, componentType=component, count=len(values), type=kind)
        if component == 5126:
            accessor.update(min=values.min(0).tolist(), max=values.max(0).tolist())
        self.doc['accessors'].append(accessor)
        return len(self.doc['accessors']) - 1

    def write(self, directory):
        self.doc['buffers'][0].update(uri='Samoyed.bin', byteLength=len(self.data))
        (directory / 'Samoyed.bin').write_bytes(self.data)
        (directory / 'Samoyed.gltf').write_text(json.dumps(self.doc, indent=2) + '\n')


def smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def sculpt_foreface(positions, head_influence, lip_tuck, nose_height, jaw_influence=None):
    """Local Samoyed form study, in metres in the frozen source's rest frame.

    Tuck the projecting upper lip into the closed mouth, carrying adjacent
    tissue smoothly. The nasal height study changes both the alae and their
    openings instead of substituting a painted nose. The jaw remains authored.
    """
    p = positions.astype(float).copy()
    front = smoothstep((p[:, 2] - .390) / .040)
    # The upper lip is below the nasal pad (minimum Y .52877 m in the
    # authored source). Finish the falloff before that pad: extending it to
    # .544 moved the lower nose by over 8 mm whenever the mouth closed.
    low = 1.0 - smoothstep((p[:, 1] - .512) / .016)
    mouth = front * low * head_influence
    # The closed upper margin sits outside and below the mandible. Carry it
    # inward as well as upward: just pitching the jaw opens this crescent gap.
    p[:, 0] -= .90 * lip_tuck * np.clip(p[:, 0] / .020, -1.0, 1.0) * mouth
    p[:, 1] += .90 * lip_tuck * mouth
    p[:, 2] -= 1.20 * lip_tuck * smoothstep((p[:, 2] - .466) / .025) * mouth
    if jaw_influence is not None:
        # The closed upper margin is tucked upward, while the lower chin was
        # left hanging beneath it. Carry only the soft underside of the jaw
        # into that closed pose, fading out below the dental/lip margin. The
        # MouthSeal target is zero throughout Pant, preserving the open jaw.
        chin = front * (1.0 - smoothstep((positions[:, 1] - .501) / .017)) * jaw_influence
        p[:, 1] += .65 * lip_tuck * chin
        p[:, 0] -= .20 * lip_tuck * np.clip(positions[:, 0] / .020, -1.0, 1.0) * chin
    nasal = smoothstep((p[:, 2] - .486) / .016)
    nasal *= 1.0 - smoothstep(np.abs(p[:, 1] - .544) / .022)
    nasal *= 1.0 - smoothstep((np.abs(p[:, 0]) - .018) / .016)
    p[:, 1] -= (1.0 - nose_height) * (p[:, 1] - .544) * nasal
    return p


def sculpt_normals(positions, normals, head_influence, lip_tuck, nose_height, jaw_influence=None):
    epsilon = 1.e-5
    columns = [(sculpt_foreface(positions + axis * epsilon, head_influence, lip_tuck, nose_height, jaw_influence)
                - sculpt_foreface(positions - axis * epsilon, head_influence, lip_tuck, nose_height, jaw_influence)) / (2.0 * epsilon)
               for axis in np.eye(3)]
    jacobian = np.stack(columns, axis=2)
    assert np.linalg.det(jacobian).min() > .1, 'Folded foreface study'
    transformed = np.linalg.solve(jacobian.transpose(0, 2, 1), normals[..., None])[..., 0]
    transformed /= np.linalg.norm(transformed, axis=1, keepdims=True)
    return transformed


def quaternion_product(a, b):
    """Hamilton product in glTF's xyzw convention, with array broadcasting."""
    a, b = np.asarray(a), np.asarray(b)
    vector = a[..., 3:4] * b[..., :3] + b[..., 3:4] * a[..., :3] + np.cross(a[..., :3], b[..., :3])
    scalar = a[..., 3:4] * b[..., 3:4] - np.sum(a[..., :3] * b[..., :3], axis=-1, keepdims=True)
    return np.concatenate((vector, scalar), axis=-1)


def lay_tail(gltf, degrees, bone='tail_01', world_axis=(1.0, 0.0, 0.0)):
    """Carry the plume forward over the back, preserving the authored wag."""
    if degrees == 0.0:
        return
    doc = gltf.doc
    skin = doc['skins'][0]
    joint = next(i for i, node in enumerate(skin['joints']) if doc['nodes'][node]['name'] == bone)
    node = skin['joints'][joint]
    rest = np.linalg.inv(gltf.read(skin['inverseBindMatrices'])[joint].reshape(4, 4).T)
    axis = rest[:3, :3].T @ np.asarray(world_axis)
    axis /= np.linalg.norm(axis)
    half = math.radians(degrees) / 2.0
    offset = np.r_[axis * math.sin(half), math.cos(half)]
    for animation in doc['animations']:
        channel = next(ch for ch in animation['channels'] if ch['target'] == dict(node=node, path='rotation'))
        sampler = animation['samplers'][channel['sampler']]
        assert sampler.get('interpolation', 'LINEAR') in ('LINEAR', 'STEP')
        rotations = quaternion_product(gltf.read(sampler['output']), offset)
        rotations /= np.linalg.norm(rotations, axis=1, keepdims=True)
        # Give this channel its own sampler: a constant rest accessor/sampler
        # may be shared with another bone by the exporter.
        animation['samplers'].append(dict(sampler, output=gltf.append(rotations, 'VEC4')))
        channel['sampler'] = len(animation['samplers']) - 1


def deform_lid(positions, normals, eye, upper, side, closure):
    centre = np.asarray(eye['centre'])
    gaze, up = np.asarray(eye['gaze']), np.asarray(eye['up'])
    axis = np.cross(up, gaze)
    axis /= np.linalg.norm(axis)
    relative = positions - centre
    radius = np.linalg.norm(relative, axis=1)
    psi = np.arcsin(np.clip(relative @ axis / radius, -1.0, 1.0))
    phi = np.arctan2(relative @ up, relative @ gaze)
    fissure = np.radians(np.where(psi * side > 0.0, OUTER_FISSURE, INNER_FISSURE))
    t = np.minimum(np.abs(psi) / fissure, 1.0)
    outer = np.clip(psi / fissure, -1.0, 1.0) * side
    shape = (1.0 - t ** 1.35) ** .85 if upper else 1.0 - t ** 2.7
    top = UPPER_OPEN if upper else LOWER_OPEN
    cross = 6.0 * smoothstep((np.abs(psi) / fissure - 1.0) / .14) * (-1.0 if upper else 1.0)
    margin = np.radians(CANTHUS + (top - CANTHUS) * shape - OUTER_DROOP * .5 * outer + cross)
    # Both margins converge onto the same curved crease. Closure goes to zero
    # at the canthi, unlike a rigid rotation of the complete shell.
    meet = np.radians(CANTHUS + (MEET - CANTHUS) * (1.0 - t * t) - OUTER_DROOP * .5 * outer + cross)
    behind_margin = np.abs(phi - margin)
    attachment = 1.0 - smoothstep((behind_margin - math.radians(22.0)) / math.radians(70.0))
    delta = (meet - margin) * closure * attachment
    new_phi = phi + delta
    direction = (np.cos(psi)[:, None] * (np.cos(new_phi)[:, None] * gaze + np.sin(new_phi)[:, None] * up)
                 + np.sin(psi)[:, None] * axis)
    deformed = centre + radius[:, None] * direction
    # The lid's outer face remains on the sphere; rotate the authored normal,
    # including the small rounded margin, through the same angular displacement.
    angle = -delta
    rotated_normal = (normals * np.cos(angle)[:, None]
                      + np.cross(axis, normals) * np.sin(angle)[:, None]
                      + (normals @ axis)[:, None] * axis * (1.0 - np.cos(angle))[:, None])
    corners = np.abs(psi) >= fissure
    if corners.any():
        assert np.linalg.norm(deformed[corners] - positions[corners], axis=1).max() < 1.e-6
    return deformed, rotated_normal


def basis_weights(amount):
    """Piecewise interpolation between three poses keeps the globe clearance."""
    nodes = np.arange(1, BLINK_STEPS + 1) / BLINK_STEPS
    return np.maximum(1.0 - np.abs(np.asarray(amount)[:, None] - nodes) * BLINK_STEPS, 0.0)


def deform_orbit(positions, eye, eye_radius, side, closure):
    """Carry the furred socket edge with its lid, fading into the cheek/brow.

    A stationary aperture around a moving shell creates a separate visible cap.
    This field moves the nearby facial tissue on the same angular path, while
    preserving distance from the globe and leaving the canthi stationary.
    """
    centre = np.asarray(eye['centre'])
    gaze, up = np.asarray(eye['gaze']), np.asarray(eye['up'])
    axis = np.cross(up, gaze)
    axis /= np.linalg.norm(axis)
    relative = positions - centre
    radius = np.linalg.norm(relative, axis=1)
    psi = np.arcsin(np.clip(relative @ axis / radius, -1.0, 1.0))
    phi = np.arctan2(relative @ up, relative @ gaze)
    fissure = np.radians(np.where(psi * side > 0.0, OUTER_FISSURE, INNER_FISSURE))
    t = np.minimum(np.abs(psi) / fissure, 1.0)
    outer = np.clip(psi / fissure, -1.0, 1.0) * side
    centre_line = np.radians(CANTHUS - OUTER_DROOP * .5 * outer)
    upper_margin = centre_line + math.radians(UPPER_OPEN - CANTHUS) * (1.0 - t ** 1.35) ** .85
    lower_margin = centre_line + math.radians(LOWER_OPEN - CANTHUS) * (1.0 - t ** 2.7)
    meet = centre_line + math.radians(MEET - CANTHUS) * (1.0 - t * t)
    # The field is monotone through the aperture, including the socket's
    # backing surface. A binary upper/lower switch folds that backing surface.
    margin = np.clip(phi, lower_margin, upper_margin)
    distance = np.hypot(radius * np.abs(phi - margin), np.maximum(radius - eye_radius - .0012, 0.0))
    influence = (1.0 - smoothstep(distance / .023)) * smoothstep((relative @ gaze) / .005)
    influence *= (t < 1.0)
    # The lower orbital tissue can follow the lid; the muzzle/lip support below
    # it must remain in its authored oral pose.
    influence *= smoothstep(((relative @ up) + .026) / .008)
    # Leave a narrow crease for the actual lid margin rather than collapsing
    # facial triangles to zero area when the eye closes completely.
    new_phi = phi + (meet - margin) * influence * closure * .92
    direction = (np.cos(psi)[:, None] * (np.cos(new_phi)[:, None] * gaze + np.sin(new_phi)[:, None] * up)
                 + np.sin(psi)[:, None] * axis)
    return centre + radius[:, None] * direction


def orbit_normals(positions, normals, eye, eye_radius, side, closure):
    # Transform normals by the deformation gradient, including its spatial
    # falloff. A rigid normal rotation would shade this soft transition wrong.
    columns = []
    epsilon = 1.e-5
    for axis in np.eye(3):
        columns.append((deform_orbit(positions + axis * epsilon, eye, eye_radius, side, closure)
                        - deform_orbit(positions - axis * epsilon, eye, eye_radius, side, closure)) / (2.0 * epsilon))
    jacobian = np.stack(columns, axis=2)
    determinant = np.linalg.det(jacobian)
    assert determinant.min() > .05, ('Folded orbital surface', float(determinant.min()), positions[determinant.argmin()].tolist())
    transformed = np.linalg.solve(jacobian.transpose(0, 2, 1), normals[..., None])[..., 0]
    transformed /= np.linalg.norm(transformed, axis=1, keepdims=True)
    return transformed


def foreface_form(positions, shortening, breadth):
    """Shorten the foreface as one dental/oral assembly; retain the eye seats."""
    p = positions.astype(float)
    q = p.copy()
    muzzle = smoothstep((p[:, 2] - .434) / .065)
    q[:, 2] -= shortening * muzzle
    width = smoothstep((p[:, 2] - .433) / .030) * (1.0 - smoothstep((p[:, 2] - .476) / .026))
    q[:, 0] *= 1.0 + breadth * width
    return q


def foreface_normals(positions, normals, shortening, breadth):
    epsilon = 1.e-5
    columns = [(foreface_form(positions + axis * epsilon, shortening, breadth)
                - foreface_form(positions - axis * epsilon, shortening, breadth)) / (2.0 * epsilon)
               for axis in np.eye(3)]
    jacobian = np.stack(columns, axis=2)
    assert np.linalg.det(jacobian).min() > .3, 'Folded foreface'
    transformed = np.linalg.solve(jacobian.transpose(0, 2, 1), normals[..., None])[..., 0]
    transformed /= np.linalg.norm(transformed, axis=1, keepdims=True)
    return transformed


def build(source, output, lip_tuck=LIP_TUCK, nose_height=NOSE_HEIGHT, tail_lay=TAIL_LAY,
          tail_roll=TAIL_ROLL, tail_bend=TAIL_BEND, muzzle_shortening=MUZZLE_SHORTENING,
          muzzle_breadth=MUZZLE_BREADTH):
    source, output = source.resolve(), output.resolve()
    assert source != output and source not in output.parents, 'Use a separate output directory'
    if output.exists() and any(output.iterdir()):
        raise FileExistsError(f'Output directory is not empty: {output}')
    rig = json.loads((source / 'Samoyed.rig.json').read_text())
    assert rig['breed'] == 'samoyed', 'This authoring script is exclusively for the Samoyed'
    # A facial revision can reseat the eyes without moving the muzzle. Sources
    # with that revision provide the head's unchanged sculpt frame explicitly.
    face_frame = rig.get('faceSculptAnchor', np.mean([eye['centre'] for eye in rig['eyes'].values()], axis=0))
    face_offset = np.asarray(face_frame) - FACE_ANCHOR
    gltf = Gltf(source / 'Samoyed.gltf')
    doc = gltf.doc
    assert 0.0 <= tail_lay <= 75.0 and -40.0 <= tail_roll <= 40.0 and 0.0 <= tail_bend <= 30.0
    assert 0.0 <= muzzle_shortening <= .020 and 0.0 <= muzzle_breadth <= .15
    lay_tail(gltf, tail_lay)
    lay_tail(gltf, tail_roll, world_axis=(0.0, 0.0, 1.0))
    lay_tail(gltf, tail_bend, bone='tail_02')
    skin = doc['skins'][0]
    names = [doc['nodes'][node]['name'] for node in skin['joints']]
    head = names.index('head')
    body_node = next(i for i, node in enumerate(doc['nodes']) if 'mesh' in node and 'skin' in node)
    mesh = doc['meshes'][doc['nodes'][body_node]['mesh']]
    assert not mesh.get('weights') and not any(p.get('targets') for p in mesh['primitives'])
    assert 0.0 <= lip_tuck <= .008 and .5 <= nose_height <= 1.0
    sculpts = []
    if lip_tuck != 0.0:
        sculpts.append(('MouthSeal', lip_tuck, 1.0))
    if nose_height != 1.0:
        sculpts.append(('NoseShape', 0.0, nose_height))
    if muzzle_shortening != 0.0 or muzzle_breadth != 0.0:
        sculpts.append(('SamoyedForeface', 0.0, 1.0))
    blink_count = 2 * BLINK_STEPS
    target_count = blink_count + len(sculpts)
    proof = dict(lidVertices=0, orbitalVertices=0, minimumInterpolatedGlobeClearanceMM=1000.0, clips={})
    for primitive in mesh['primitives']:
        attr = primitive['attributes']
        positions, normals = gltf.read(attr['POSITION']), gltf.read(attr['NORMAL'])
        joints, weights = gltf.read(attr['JOINTS_0']), gltf.read(attr['WEIGHTS_0'])
        dominant = joints[np.arange(len(positions)), weights.argmax(1)]
        deltas = [np.zeros_like(positions) for _ in range(target_count)]
        normal_deltas = [np.zeros_like(normals) for _ in range(target_count)]
        material_name = doc['materials'][primitive['material']]['name']
        for side_index, side_name in enumerate(('L', 'R')):
            lid_bones = [i for i, name in enumerate(names) if name.startswith('lid_')]
            near = (np.linalg.norm(positions - rig['eyes'][side_name]['centre'], axis=1) < .050)
            near &= ~np.isin(dominant, lid_bones)
            near &= material_name == 'DogSkin'
            near &= np.isin(dominant, [names.index(name) for name in ('head', 'brow_L', 'brow_R')])
            if near.any():
                proof['orbitalVertices'] += int(near.sum())
                for step in range(BLINK_STEPS):
                    args = (rig['eyes'][side_name], rig['eyeRadius'], 1 if side_name == 'L' else -1,
                            (step + 1) / BLINK_STEPS)
                    target = side_index * BLINK_STEPS + step
                    deltas[target][near] = deform_orbit(positions[near], *args) - positions[near]
                    normal_deltas[target][near] = orbit_normals(positions[near], normals[near], *args) - normals[near]
            for upper in (True, False):
                bone = names.index(f'lid_{"upper" if upper else "lower"}_{side_name}')
                selected = dominant == bone
                if not selected.any():
                    continue
                assert np.all(weights[selected].max(1) > .999), 'Unexpected blended source lid'
                proof['lidVertices'] += int(selected.sum())
                poses = []
                for step in range(BLINK_STEPS):
                    deformed, normal = deform_lid(positions[selected], normals[selected], rig['eyes'][side_name],
                                                  upper, 1 if side_name == 'L' else -1, (step + 1) / BLINK_STEPS)
                    target = side_index * BLINK_STEPS + step
                    deltas[target][selected] = deformed - positions[selected]
                    normal_deltas[target][selected] = normal - normals[selected]
                    poses.append(deformed - positions[selected])
                # Check actual interpolated target poses, including the inner
                # surface and margin, against the eyeball for the whole blink.
                amounts = np.linspace(0.0, 1.0, 61)
                for amount in amounts:
                    deformed = positions[selected] + np.einsum('k,kij->ij', basis_weights([amount])[0], np.asarray(poses))
                    clearance = np.linalg.norm(deformed - rig['eyes'][side_name]['centre'], axis=1) - rig['eyeRadius']
                    proof['minimumInterpolatedGlobeClearanceMM'] = min(proof['minimumInterpolatedGlobeClearanceMM'], float(clearance.min() * 1000.0))
                joints[selected] = head
                weights[selected] = (1.0, 0.0, 0.0, 0.0)
        # Replace lid influences only; the entire rest vertex and index stream stays intact.
        if material_name in ('DogNose', 'DogGum', 'DogTongue', 'DogTeeth', 'DogPad'):
            assert all(np.count_nonzero(delta) == 0 for delta in deltas), material_name
        if material_name == 'DogLip':
            oral = ~np.isin(dominant, lid_bones)
            assert all(np.count_nonzero(delta[oral]) == 0 for delta in deltas), 'The blink moved an oral lip'
        for sculpt_index, (sculpt_name, tuck, height) in enumerate(sculpts, blink_count):
            facial_positions = positions - face_offset
            if sculpt_name == 'SamoyedForeface':
                deltas[sculpt_index] = foreface_form(facial_positions, muzzle_shortening, muzzle_breadth) - facial_positions
                normal_deltas[sculpt_index] = foreface_normals(facial_positions, normals, muzzle_shortening, muzzle_breadth) - normals
                continue
            if material_name in ('DogTeeth', 'DogTongue'):
                continue
            head_influence = np.sum(weights * (joints == head), axis=1)
            jaw_influence = np.sum(weights * (joints == names.index('jaw')), axis=1)
            # Carry adjoining soft tissue across skin/lip/gum boundaries. Teeth
            # and tongue keep their authored dental clearance and rig motion.
            deltas[sculpt_index] = sculpt_foreface(facial_positions, head_influence, tuck, height, jaw_influence) - facial_positions
            normal_deltas[sculpt_index] = sculpt_normals(facial_positions, normals, head_influence, tuck, height, jaw_influence) - normals
            if sculpt_name == 'MouthSeal' and material_name == 'DogNose':
                displacement = float(np.linalg.norm(deltas[sculpt_index], axis=1).max())
                assert displacement < 1.e-8, 'Closing the lips must not reshape the nasal pad'
                proof['mouthSealNasalDisplacementMM'] = displacement * 1000.0
        attr['JOINTS_0'] = gltf.append(joints, 'VEC4', doc['accessors'][attr['JOINTS_0']]['componentType'])
        attr['WEIGHTS_0'] = gltf.append(weights, 'VEC4')
        primitive['targets'] = [dict(POSITION=gltf.append(delta, 'VEC3'), NORMAL=gltf.append(normal, 'VEC3'))
                                for delta, normal in zip(deltas, normal_deltas)]
    assert proof['minimumInterpolatedGlobeClearanceMM'] > .1, proof
    proof['blinkLeavesOralMaterialsAndLipVerticesUnchanged'] = True
    mesh['weights'] = [0.0] * target_count
    mesh.setdefault('extras', {})['targetNames'] = [f'Blink_{side}_{step}' for side in ('L', 'R') for step in range(1, BLINK_STEPS + 1)]
    for index, (name, _, _) in enumerate(sculpts, blink_count):
        mesh['weights'][index] = 1.0
        mesh['extras']['targetNames'].append(name)
    lid_nodes = {node for name, node in zip(names, skin['joints']) if name.startswith('lid_')}
    for animation in doc['animations']:
        original = copy.deepcopy(animation)
        seconds = max(float(gltf.read(s['input']).max()) for s in original['samplers'])
        times = np.linspace(0.0, seconds, int(round(seconds * 60)) + 1)
        morph = np.zeros((len(times), target_count))
        for index, (name, _, _) in enumerate(sculpts, blink_count):
            if name == 'MouthSeal':
                jaw = skin['joints'][names.index('jaw')]
                channel = next(ch for ch in original['channels'] if ch['target'] == dict(node=jaw, path='rotation'))
                sampler = original['samplers'][channel['sampler']]
                interpolation = sampler.get('interpolation', 'LINEAR')
                assert interpolation in ('LINEAR', 'STEP')
                rotations = gltf.read(sampler['output']).astype(float)
                rotations /= np.linalg.norm(rotations, axis=1, keepdims=True)
                rest = np.array(doc['nodes'][jaw].get('rotation', [0, 0, 0, 1]), dtype=float)
                rest /= np.linalg.norm(rest)
                angles = 2.0 * np.arccos(np.clip(np.abs(rotations @ rest), 0.0, 1.0))
                closure = 1.0 - smoothstep(angles / math.radians(12.0))
                key_times = gltf.read(sampler['input']).ravel()
                morph[:, index] = (closure[np.maximum(np.searchsorted(key_times, times, side='right') - 1, 0)]
                                   if interpolation == 'STEP' else np.interp(times, key_times, closure))
            else:
                morph[:, index] = 1.0
        for side_index, side in enumerate(('L', 'R')):
            node = skin['joints'][names.index('lid_upper_' + side)]
            channel = next(ch for ch in original['channels'] if ch['target'] == dict(node=node, path='rotation'))
            sampler = original['samplers'][channel['sampler']]
            interpolation = sampler.get('interpolation', 'LINEAR')
            assert interpolation in ('LINEAR', 'STEP'), interpolation
            rotations = gltf.read(sampler['output']).astype(float)
            rotations /= np.linalg.norm(rotations, axis=1, keepdims=True)
            rest = np.array(doc['nodes'][node].get('rotation', [0, 0, 0, 1]), dtype=float)
            rest /= np.linalg.norm(rest)
            amounts = 2.0 * np.arccos(np.clip(np.abs(rotations @ rest), 0.0, 1.0)) / math.radians(UPPER_OPEN - MEET)
            assert amounts.max() <= 1.002, (animation['name'], side, amounts.max())
            amounts = np.clip(amounts, 0.0, 1.0)
            amounts[amounts < 1.e-5] = 0.0
            key_times = gltf.read(sampler['input']).ravel()
            sampled = (amounts[np.maximum(np.searchsorted(key_times, times, side='right') - 1, 0)]
                       if interpolation == 'STEP' else np.interp(times, key_times, amounts))
            morph[:, side_index * BLINK_STEPS:(side_index + 1) * BLINK_STEPS] = basis_weights(sampled)
        # Static original lid bones keep their names/sockets; they no longer
        # deform the moving surface or need animation channels.
        animation['channels'] = [ch for ch in original['channels'] if ch['target']['node'] not in lid_nodes]
        animation['samplers'] = original['samplers']
        index = len(animation['samplers'])
        animation['samplers'].append(dict(input=gltf.append(times, 'SCALAR'),
                                          output=gltf.append(morph.ravel(), 'SCALAR'), interpolation='LINEAR'))
        animation['channels'].append(dict(sampler=index, target=dict(node=body_node, path='weights')))
        proof['clips'][animation['name']] = dict(seconds=seconds, sampledFrames=len(times),
                                                maxTargetWeights=morph.max(0).tolist(),
                                                loopWeightError=float(np.abs(morph[-1] - morph[0]).max()))
    output.mkdir(parents=True, exist_ok=True)
    manifest = dict(source=str(source), inputs={}, checks=proof,
                    sculpt=dict(lipTuckMetres=lip_tuck, noseHeight=nose_height, tailLayDegrees=tail_lay,
                                tailRollDegrees=tail_roll, tailBendDegrees=tail_bend,
                                muzzleShorteningMetres=muzzle_shortening, muzzleBreadth=muzzle_breadth,
                                faceFrameOffsetMetres=face_offset.tolist()),
                    acceptance='Unaccepted authoring candidate; inspect actual engine frames')
    for path in source.iterdir():
        if path.is_file() and path.suffix in ('.gltf', '.bin', '.json', '.png', '.abc'):
            manifest['inputs'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
            shutil.copy2(path, output / path.name)
    rig['assembly']['lid'] = rig['assembly']['pelt']
    rig['eyeProfile'] = 'Models/Samoyed/SamoyedEye.oloskin'
    eye_profile = Path(__file__).with_name('SamoyedEye.oloskin')
    shutil.copy2(eye_profile, output / eye_profile.name)
    manifest['eyeProfileSHA256'] = hashlib.sha256(eye_profile.read_bytes()).hexdigest()
    (output / 'Samoyed.rig.json').write_text(json.dumps(rig, indent=2) + '\n')
    gltf.write(output)
    manifest['scriptSHA256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(proof, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--lip-tuck', type=float, default=LIP_TUCK, help='Closed upper-lip contact amount, metres')
    parser.add_argument('--nose-height', type=float, default=NOSE_HEIGHT, help='Nasal height fraction')
    parser.add_argument('--tail-lay', type=float, default=TAIL_LAY, help='Forward plume carriage, degrees')
    parser.add_argument('--tail-roll', type=float, default=TAIL_ROLL, help='Sideways plume carriage, degrees')
    parser.add_argument('--tail-bend', type=float, default=TAIL_BEND, help='Distal plume bend, degrees')
    parser.add_argument('--muzzle-shortening', type=float, default=MUZZLE_SHORTENING, help='Foreface shortening, metres')
    parser.add_argument('--muzzle-breadth', type=float, default=MUZZLE_BREADTH, help='Whisker-pad width fraction')
    args = parser.parse_args()
    build(args.source, args.output, args.lip_tuck, args.nose_height, args.tail_lay,
          args.tail_roll, args.tail_bend, args.muzzle_shortening, args.muzzle_breadth)

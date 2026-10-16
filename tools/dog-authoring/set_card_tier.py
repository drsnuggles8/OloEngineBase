"""Re-author the card tier an Alembic coat source carries, in place of re-exporting it from Blender.

Example:

    python tools/dog-authoring/set_card_tier.py --cell 0.006 \
        OloEditor/SandboxProject/Assets/Models/Bernese/Bernese.abc

A breed's coat is cooked from its committed archive (Assets/Models/<breed>/<breed>.abc), and every hair
object in it carries the card tier as two user properties, `groom_card_cell_size` (root-UV cell) and
`groom_card_points` (AlembicGroomImporter.h). The importer cooks the tier the archive authors, so the
editor's "Import as Groom" and the shipped coat agree; changing the tier means changing the archive.
Re-exporting it from Blender is not an option for a breed: Blender writes the hair objects in a new
order on every export, and the importer numbers the groups in archive order, so a re-export is the
same coat in another curve order.

This edits the archive itself. Alembic's Ogawa container stores each property's sample as a data
block that the writer shares between equal values (the Bernese's cell shares its block with a wave
amplitude of the same value), so the new values are appended as blocks of their own and only the
card properties are pointed at them: every other byte of the archive keeps its place. The blocks'
16-byte keys are the writer's de-duplication digests, which readers never check. The archive is then
re-cooked (OLO_DOG_EXPORT=1, or rebuild.py's native cook) and the cooked coat ships with it.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

DATA = 1 << 63  # an Ogawa child position with this bit set is a data block, else a group


class Archive:
    def __init__(self, data):
        if data[:5] != b'Ogawa':
            raise ValueError('not an Ogawa Alembic archive')
        self.d = data
        self.root = struct.unpack_from('<Q', data, 8)[0]

    def group(self, position):
        if position == 0:
            return []
        count = struct.unpack_from('<Q', self.d, position)[0]
        return list(struct.unpack_from(f'<{count}Q', self.d, position + 8))

    def data(self, position):
        if not position & DATA:
            raise ValueError('expected a data block')
        p = position & ~DATA
        if p == 0:
            return b''
        size = struct.unpack_from('<Q', self.d, p)[0]
        return bytes(self.d[p + 8:p + 8 + size])


def _hinted(buf, pos, hint):
    if hint == 0:
        return buf[pos], pos + 1
    if hint == 1:
        return struct.unpack_from('<H', buf, pos)[0], pos + 2
    return struct.unpack_from('<I', buf, pos)[0], pos + 4


def property_headers(buf):
    """AbcCoreOgawa's packed property headers: (name, type, pod) per property, in group order."""
    pos, out = 0, []
    while pos < len(buf):
        info = struct.unpack_from('<I', buf, pos)[0]
        pos += 4
        ptype, hint = info & 0x3, (info & 0xc) >> 2
        if ptype != 0:
            _, pos = _hinted(buf, pos, hint)  # sample count
            if info & 0x200:
                _, pos = _hinted(buf, pos, hint)
                _, pos = _hinted(buf, pos, hint)
            if info & 0x100:
                _, pos = _hinted(buf, pos, hint)
        size, pos = _hinted(buf, pos, hint)
        name = buf[pos:pos + size].decode()
        pos += size
        if (info & 0xff00000) >> 20 == 0xff:
            size, pos = _hinted(buf, pos, hint)
            pos += size
        out.append((name, ptype, (info & 0xf0) >> 4))
    return out


def object_names(buf):
    pos, out = 0, []
    while pos < len(buf) - 32:  # the last 32 bytes are the object's property and child digests
        size = struct.unpack_from('<I', buf, pos)[0]
        out.append(buf[pos + 4:pos + 4 + size].decode())
        pos += 4 + size
        meta = buf[pos]
        pos += 1
        if meta == 0xff:
            pos += 4 + struct.unpack_from('<I', buf, pos)[0]
    return out


def properties(archive):
    """Every non-compound property: (path, pod, group position)."""
    found = []

    def compound(position, path):
        children = archive.group(position)
        if not children:
            return
        for (name, ptype, pod), child in zip(property_headers(archive.data(children[-1])), children[:-1]):
            if ptype == 0:
                compound(child, f'{path}/{name}')
            else:
                found.append((f'{path}/{name}', pod, child))

    def obj(position, path):
        children = archive.group(position)
        if not children:
            return
        compound(children[0], path + '.')
        for name, child in zip(object_names(archive.data(children[-1])), children[1:-1]):
            obj(child, f'{path}/{name}')

    obj(archive.group(archive.root)[2], '')
    return found


POD = {'groom_card_cell_size': (11, '<d'), 'groom_card_points': (6, '<i')}  # Alembic kFloat64POD, kInt32POD


def set_card_tier(path, cell, points):
    data = bytearray(path.read_bytes())
    archive = Archive(data)
    values = {'groom_card_cell_size': cell, 'groom_card_points': points}
    changed = {}
    for name, value in values.items():
        if value is None:
            continue
        pod, fmt = POD[name]
        targets = [(p, g) for p, kind, g in properties(archive) if p.endswith('/' + name)]
        if not targets:
            raise ValueError(f'{path} authors no {name}: build_dog_groom.py writes it on every hair object')
        payload = struct.pack(fmt, value)
        block = hashlib.md5(payload).digest() + payload
        position = len(data)
        data.extend(struct.pack('<Q', len(block)) + block)
        for p, group in targets:
            if archive.group(group)[0] & DATA == 0:
                raise ValueError(f'{p}: not a one-sample property')
            struct.pack_into('<Q', data, group + 8, position | DATA)  # the group's first child: sample 0
        changed[name] = len(targets)
    path.write_bytes(data)
    check = Archive(bytes(data))
    for name, value in values.items():
        if value is None:
            continue
        seen = {struct.unpack_from(POD[name][1], check.data(check.group(g)[0]), 16)[0]
                for p, _, g in properties(check) if p.endswith('/' + name)}
        if seen != {value}:
            raise RuntimeError(f'{path}: {name} reads back {seen}, not {value}')
    print(f'{path}: ' + ', '.join(f'{n} = {values[n]} on {c} objects' for n, c in changed.items()))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cell', type=float, help='groom_card_cell_size, in root-UV units')
    parser.add_argument('--points', type=int, help='groom_card_points, control points per card')
    parser.add_argument('archives', nargs='+', type=Path)
    args = parser.parse_args()
    if args.cell is None and args.points is None:
        parser.error('nothing to set: pass --cell and/or --points')
    for archive_path in args.archives:
        set_card_tier(archive_path, args.cell, args.points)

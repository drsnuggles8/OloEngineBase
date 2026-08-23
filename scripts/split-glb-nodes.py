#!/usr/bin/env python3
"""Split a binary glTF (.glb) into per-part files, one named node subtree each.

WHY THIS EXISTS (issue #899)
    `Model` flattens the imported node graph under `aiProcess_PreTransformVertices`
    and draws every submesh with the entity's single transform, so a part of a
    `ModelComponent` cannot be posed independently of the rest. When a part of a
    model has to animate on its own -- a sail that trims, a turret that traverses,
    a door that opens -- the answer is to split the source file and give the part
    its own entity.

    Drift's `ship-small.glb` was split with this into `ship-small-hull.glb` (the
    hull plus both flags) and `ship-small-sail.glb` (the rig, re-origined onto its
    mast), which is what let `DriftBoatController.lua` brace the yard.

THE RE-ORIGIN IS THE LOAD-BEARING PART
    Node transforms are baked into the vertices, so an extracted part's geometry
    is still sitting at its position inside the whole model. Rotating an entity
    carrying it swings the part around the MODEL's origin rather than around its
    own hinge -- it still moves, plausibly, about the wrong point. `--re-origin`
    drops the extracted root node's translation so its own pivot lands on the file
    origin; the scene then places the entity at exactly that translation in the
    parent's space and the split is invisible.

    The tool prints the dropped translation for each re-origined part, because that
    is the number the scene has to author.

USAGE
    # the part that moves, pivoted on its own hinge
    python scripts/split-glb-nodes.py ship-small.glb --keep sail-a --re-origin \
        --out ship-small-sail.glb

    # everything else, left exactly where it was
    python scripts/split-glb-nodes.py ship-small.glb --keep ship-small --drop sail-a \
        --out ship-small-hull.glb

WHAT IT PRESERVES
    Geometry, UVs, materials, textures and image URIs are untouched. Only the node
    list, the accessor/bufferView packing (repacked to just what the surviving
    meshes reach) and -- with `--re-origin` -- one translation differ. Files that
    reference an EXTERNAL texture keep referencing it, so a split pair costs no
    extra texture memory.

LIMITS
    Rejects sparse accessors and morph targets rather than mangling them, and does
    not carry skins or animations. Those have never been needed here; extend it
    deliberately if they are.

AFTERWARDS
    `.glb` is a supported asset extension, so a new one under a project's
    `Assets/` must reach `AssetRegistry.oar` or
    `AssetContentValidity.EverySupportedAssetOnDiskIsInTheRegistry` fails.
    Launching OloEditor once rescans and adds it; commit the updated `.oar`.
"""

import argparse
import json
import struct
import sys


def read_glb(path):
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise SystemExit(f"{path}: not a binary glTF")
    off = 12
    js, bin_chunk = None, b""
    while off < len(data):
        length, ctype = struct.unpack_from("<II", data, off)
        off += 8
        payload = data[off:off + length]
        off += length
        if ctype == 0x4E4F534A:
            js = json.loads(payload)
        elif ctype == 0x004E4942:
            bin_chunk = payload
    if js is None:
        raise SystemExit(f"{path}: no JSON chunk")
    return js, bin_chunk


def _pad4(b, filler=b"\x00"):
    while len(b) % 4:
        b += filler
    return b


def write_glb(path, js, bin_chunk):
    js_bytes = _pad4(json.dumps(js, separators=(",", ":")).encode("utf-8"), b" ")
    bin_bytes = _pad4(bin_chunk)
    total = 12 + 8 + len(js_bytes) + (8 + len(bin_bytes) if bin_bytes else 0)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(js_bytes), 0x4E4F534A) + js_bytes
    if bin_bytes:
        out += struct.pack("<II", len(bin_bytes), 0x004E4942) + bin_bytes
    with open(path, "wb") as f:
        f.write(out)
    return total


def subset(js, bin_chunk, keep_names, drop_names, re_origin):
    """A new glTF holding the `keep_names` subtrees, minus any `drop_names`."""
    src_nodes = js["nodes"]
    picked = []
    for name in keep_names:
        matches = [i for i, n in enumerate(src_nodes) if n.get("name") == name]
        if not matches:
            raise SystemExit(f"no node named {name!r}; have "
                             f"{sorted(n.get('name') for n in src_nodes)}")
        if len(matches) > 1:
            raise SystemExit(f"node name {name!r} is not unique")
        picked.append(matches[0])

    # Materials, textures, samplers and images are small and shared; carrying
    # them wholesale keeps every part looking identical to the original and keeps
    # an external atlas reference intact.
    out = {"asset": dict(js["asset"])}
    for key in ("extensionsUsed", "extensionsRequired", "materials", "textures",
                "samplers", "images"):
        if js.get(key):
            out[key] = json.loads(json.dumps(js[key]))

    new_meshes, mesh_map = [], {}
    new_accessors, accessor_map = [], {}
    new_views, view_map = [], {}
    new_bin = bytearray()

    def take_view(vi):
        if vi in view_map:
            return view_map[vi]
        v = js["bufferViews"][vi]
        start = v.get("byteOffset", 0)
        blob = bin_chunk[start:start + v["byteLength"]]
        while len(new_bin) % 4:
            new_bin.append(0)
        nv = {"buffer": 0, "byteOffset": len(new_bin), "byteLength": len(blob)}
        for opt in ("byteStride", "target"):
            if opt in v:
                nv[opt] = v[opt]
        new_bin.extend(blob)
        view_map[vi] = len(new_views)
        new_views.append(nv)
        return view_map[vi]

    def take_accessor(ai):
        if ai in accessor_map:
            return accessor_map[ai]
        a = dict(js["accessors"][ai])
        if "sparse" in a:
            raise SystemExit("sparse accessors are not handled")
        if "bufferView" in a:
            a["bufferView"] = take_view(a["bufferView"])
        accessor_map[ai] = len(new_accessors)
        new_accessors.append(a)
        return accessor_map[ai]

    def take_mesh(mi):
        if mi in mesh_map:
            return mesh_map[mi]
        m = json.loads(json.dumps(js["meshes"][mi]))
        for prim in m["primitives"]:
            if "targets" in prim:
                raise SystemExit("morph targets are not handled")
            prim["attributes"] = {k: take_accessor(v) for k, v in prim["attributes"].items()}
            if "indices" in prim:
                prim["indices"] = take_accessor(prim["indices"])
        mesh_map[mi] = len(new_meshes)
        new_meshes.append(m)
        return mesh_map[mi]

    out_nodes = []
    dropped_translations = {}

    def copy_subtree(src_idx, is_root):
        n = json.loads(json.dumps(src_nodes[src_idx]))
        children = n.pop("children", [])
        if "skin" in n:
            raise SystemExit("skinned nodes are not handled")
        if "mesh" in n:
            n["mesh"] = take_mesh(n["mesh"])
        if re_origin and is_root:
            # A node may carry its transform as a `matrix` instead of TRS, and
            # dropping only "translation" would then leave the offset baked in:
            # the part would still pivot around the model origin, silently, which
            # is the exact failure this flag exists to prevent. Refuse rather than
            # half-apply.
            if "matrix" in n:
                raise SystemExit(
                    f"--re-origin: node {n.get('name')!r} uses a `matrix` transform; "
                    "decompose it to TRS first, or the re-origin would silently do nothing")
            t = n.pop("translation", None)
            if t is None:
                print(f"  note: {n.get('name')!r} had no translation to drop "
                      "(already at the file origin)")
            else:
                dropped_translations[n.get("name", f"node{src_idx}")] = t
        out_idx = len(out_nodes)
        out_nodes.append(n)
        kept = [copy_subtree(c, False) for c in children
                if src_nodes[c].get("name") not in drop_names]
        if kept:
            out_nodes[out_idx]["children"] = kept
        return out_idx

    roots = [copy_subtree(i, True) for i in picked]

    out["nodes"] = out_nodes
    out["meshes"] = new_meshes
    out["accessors"] = new_accessors
    out["bufferViews"] = new_views
    out["buffers"] = [{"byteLength": len(new_bin)}]
    out["scene"] = 0
    out["scenes"] = [{"nodes": roots}]
    return out, bytes(new_bin), dropped_translations


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="the .glb to split")
    ap.add_argument("--keep", action="append", required=True, metavar="NODE",
                    help="node to keep, with its subtree; repeatable")
    ap.add_argument("--drop", action="append", default=[], metavar="NODE",
                    help="node to omit from a kept subtree; repeatable")
    ap.add_argument("--re-origin", action="store_true",
                    help="drop each kept ROOT node's translation so its own pivot "
                         "lands on the file origin (see the module docstring)")
    ap.add_argument("--out", required=True, help="the .glb to write")
    ap.add_argument("--scene-name", default=None, help="name for the output scene")
    args = ap.parse_args(argv)

    js, bin_chunk = read_glb(args.source)
    out_js, out_bin, dropped = subset(js, bin_chunk, args.keep, set(args.drop),
                                      args.re_origin)
    if args.scene_name:
        out_js["scenes"][0]["name"] = args.scene_name

    size = write_glb(args.out, out_js, out_bin)
    print(f"{args.out}: {size} bytes, "
          f"{len(out_js['nodes'])} node(s), {len(out_js['meshes'])} mesh(es)")
    for name, t in dropped.items():
        print(f"  re-origined {name}: dropped translation {t}")
        print("  ^ author THIS as the entity's translation in the parent's space")
    return 0


if __name__ == "__main__":
    sys.exit(main())

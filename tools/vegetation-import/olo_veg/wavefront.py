"""Wavefront OBJ/MTL writer.

OBJ because that is what the repository's vegetation already is, because Assimp
resolves `map_Kd` to aiTextureType_DIFFUSE - which is the first thing
Model::LoadMaterialTextures asks for, and therefore what FoliageRenderer reads
through Material::GetAlbedoMap() to give each submesh its own texture - and
because a text mesh stays diffable, so a future change to a committed plant shows
up in review as something a human can read.

THE V FLIP HAPPENS HERE, EXACTLY ONCE. Everything upstream in this package works
in glTF convention (v = 0 at the TOP of the image, increasing downward). OBJ puts
v = 0 at the bottom. `write_obj` therefore emits `1 - v`, and no other module in
this package may flip. See gltf.py for what a double flip costs.
"""

import os

import numpy as np


def write_obj(path, name, groups, mtl_name=None):
    """Write an OBJ whose submeshes are one `usemtl` group each.

    groups: list of (material_name, positions, normals, uvs, triangles), each
    triangle array indexing into that group's own vertex arrays.

    One group per material is what gives FoliageRenderer a per-submesh albedo:
    CreateCombinedMeshSource emits one submesh per mesh, and the submesh's own
    material index picks the texture. A tree whose trunk and canopy share one
    group would take one texture for both, which is the defect issue #1398 is
    about.
    """
    mtl_name = mtl_name or (os.path.splitext(os.path.basename(path))[0] + ".mtl")
    chunks = [f"# {name} - imported by tools/vegetation-import. Do not hand-edit.\n",
              f"mtllib {mtl_name}\n", f"o {name}\n"]

    vertex_base = 0
    face_blocks = []
    for material, positions, normals, uvs, tris in groups:
        if len(tris) == 0:
            continue
        chunks.append(_format_rows("v", positions))
        chunks.append(_format_rows("vt", np.stack([uvs[:, 0], 1.0 - uvs[:, 1]], axis=1)))
        chunks.append(_format_rows("vn", normals))
        face = (tris + vertex_base + 1).astype(np.int64)
        block = [f"usemtl {material}\n", "s off\n"]
        # OBJ faces are v/vt/vn with the same index in all three slots: every
        # writer here emits one uv and one normal per position.
        cols = []
        for column in range(3):
            index = face[:, column].astype(str)
            cols.append(np.char.add(np.char.add(np.char.add(np.char.add(index, "/"), index), "/"), index))
        lines = np.char.add(np.char.add(np.char.add(np.char.add("f ", cols[0]), " "), cols[1]), " ")
        lines = np.char.add(lines, cols[2])
        block.append("\n".join(lines.tolist()) + "\n")
        face_blocks.append("".join(block))
        vertex_base += len(positions)

    chunks.extend(face_blocks)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join(chunks))
    return vertex_base


def _format_rows(tag, values):
    values = np.asarray(values, dtype=np.float64)
    text = np.char.mod("%.6g", values)
    line = np.char.add(f"{tag} ", text[:, 0])
    for column in range(1, values.shape[1]):
        line = np.char.add(np.char.add(line, " "), text[:, column])
    return "\n".join(line.tolist()) + "\n"


def write_mtl(path, materials):
    """materials: list of (name, texture_relative_path, has_alpha)."""
    lines = ["# Imported by tools/vegetation-import. Do not hand-edit.\n"]
    for name, texture, has_alpha in materials:
        lines.append(f"\nnewmtl {name}\n")
        lines.append("Ka 0 0 0\n")
        lines.append("Kd 1 1 1\n")
        lines.append("Ks 0 0 0\n")
        lines.append("d 1\n")
        lines.append("illum 1\n")
        if texture:
            texture = texture.replace(os.sep, "/")
            lines.append(f"map_Kd {texture}\n")
            if has_alpha:
                # Assimp reads map_d into aiTextureType_OPACITY. The engine takes
                # its cutout from the albedo's own alpha channel, so this line is
                # documentation for other tools rather than something OloEngine
                # consumes - the RGBA png already carries the mask.
                lines.append(f"map_d {texture}\n")
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(lines)

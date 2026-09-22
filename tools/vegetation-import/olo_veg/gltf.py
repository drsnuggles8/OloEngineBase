"""Minimal glTF 2.0 reader — accessors to numpy, no third-party dependency.

Poly Haven ships its models as .gltf + .bin + loose textures. The engine reads
those through Assimp at runtime; this reader exists for the OFFLINE import step,
which has to look at the geometry (cluster it, rasterize it, decimate it) before
anything reaches the engine at all.

Only what the importer needs is implemented: indexed triangle primitives with
POSITION / NORMAL / TEXCOORD_0, external .bin buffers, and byte-stride handling.
It deliberately does not handle .glb, sparse accessors, draco or animation — if a
source ever needs those the tool should fail loudly rather than half-read them.

TEXTURE COORDINATE ORIGIN. glTF puts UV (0,0) at the TOP-LEFT of the image and v
increases DOWNWARD; Wavefront OBJ puts it at the bottom-left with v increasing
upward. Everything in this package works in glTF convention and flips exactly
once, in wavefront.py, on the way out. Getting this wrong is not a subtle
shading difference: pine_tree_01's twig atlas has needles in the lower half and
pine cones in the upper, so a flipped v bakes brown cones onto every foliage
card and the mistake looks like a plausible dead tree.
"""

import json
import os
import urllib.parse

import numpy as np

_COMPONENT_DTYPE = {
    5120: np.int8,
    5121: np.uint8,
    5122: np.int16,
    5123: np.uint16,
    5125: np.uint32,
    5126: np.float32,
}

_TYPE_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


class GltfError(RuntimeError):
    pass


class Gltf:
    """A parsed .gltf document plus lazy access to its buffers."""

    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.dir = os.path.dirname(self.path)
        with open(self.path, "r", encoding="utf-8") as handle:
            self.doc = json.load(handle)
        self._buffers = {}

    # -- raw access ---------------------------------------------------------

    def buffer(self, index):
        if index not in self._buffers:
            spec = self.doc["buffers"][index]
            uri = spec.get("uri")
            if not uri:
                raise GltfError(f"{self.path}: buffer {index} has no uri (.glb is not supported)")
            if uri.startswith("data:"):
                raise GltfError(f"{self.path}: embedded data: buffers are not supported")
            self._buffers[index] = np.fromfile(self._resolve(uri, f"buffer {index}"), dtype=np.uint8)
        return self._buffers[index]

    def _resolve(self, uri, what):
        """Resolve a glTF-relative uri, refusing anything outside the document's directory.

        The uri comes out of a DOWNLOADED document, so it is remote input: an
        absolute path, a drive letter or a `../` chain would make the importer
        read a file elsewhere on the machine. Containment is checked on the
        RESOLVED path rather than by scanning the uri for "..", because a
        symlinked cache directory escapes without "..'" appearing anywhere.
        """
        if urllib.parse.urlsplit(uri).scheme not in ("", "file"):
            raise GltfError(f"{self.path}: {what} uri is not a relative path: {uri!r}")
        candidate = os.path.realpath(os.path.join(self.dir, urllib.parse.unquote(uri)))
        root = os.path.realpath(self.dir)
        if os.path.commonpath([candidate, root]) != root:
            raise GltfError(f"{self.path}: {what} uri escapes the asset directory: {uri!r}")
        return candidate

    def accessor(self, index):
        """Return accessor `index` as an (count, components) numpy array."""
        spec = self.doc["accessors"][index]
        if "sparse" in spec:
            raise GltfError(f"{self.path}: sparse accessor {index} is not supported")
        count = spec["count"]
        components = _TYPE_COMPONENTS[spec["type"]]
        dtype = _COMPONENT_DTYPE[spec["componentType"]]
        if "bufferView" not in spec:
            return np.zeros((count, components), dtype=dtype)

        view = self.doc["bufferViews"][spec["bufferView"]]
        raw = self.buffer(view.get("buffer", 0))
        start = view.get("byteOffset", 0) + spec.get("byteOffset", 0)
        element = np.dtype(dtype).itemsize * components
        stride = view.get("byteStride", 0) or element
        if stride == element:
            return raw[start : start + count * element].view(dtype).reshape(count, components)
        # Interleaved: gather the element bytes, then reinterpret.
        offsets = (start + np.arange(count, dtype=np.int64) * stride)[:, None] + np.arange(element)[None, :]
        return raw[offsets].copy().view(dtype).reshape(count, components)

    # -- convenience --------------------------------------------------------

    def material_name(self, index):
        if index is None:
            return None
        return self.doc["materials"][index].get("name")

    def primitives(self, mesh_index=None):
        """Yield (mesh_index, primitive_index, material_name, primitive)."""
        for mi, mesh in enumerate(self.doc.get("meshes", [])):
            if mesh_index is not None and mi != mesh_index:
                continue
            for pi, prim in enumerate(mesh.get("primitives", [])):
                yield mi, pi, self.material_name(prim.get("material")), prim

    def read_primitive(self, prim):
        """Return (positions, normals, uvs, triangles) as float32/uint32 arrays.

        `triangles` indexes into the returned vertex arrays. Primitives without a
        NORMAL attribute get zero normals; the caller decides whether to derive
        them (a foliage card gets authored normals anyway, see cards.py).
        """
        mode = prim.get("mode", 4)
        if mode != 4:
            raise GltfError(f"{self.path}: primitive mode {mode} is not TRIANGLES")
        attrs = prim["attributes"]
        positions = self.accessor(attrs["POSITION"]).astype(np.float32, copy=False)
        if "NORMAL" in attrs:
            normals = self.accessor(attrs["NORMAL"]).astype(np.float32, copy=False)
        else:
            normals = np.zeros_like(positions)
        if "TEXCOORD_0" in attrs:
            uvs = self.accessor(attrs["TEXCOORD_0"]).astype(np.float32, copy=False)
        else:
            uvs = np.zeros((len(positions), 2), dtype=np.float32)
        if "indices" in prim:
            tris = self.accessor(prim["indices"]).reshape(-1, 3).astype(np.uint32, copy=False)
        else:
            tris = np.arange(len(positions), dtype=np.uint32).reshape(-1, 3)
        return positions, normals, uvs, tris

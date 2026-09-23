"""Bake a skin THICKNESS MAP from a mesh's own geometry (issue #1394).

The thickness map of issue #1242 is a per-texel multiplier on the material's
ThicknessFactor (metres), red channel, linear [0, 1]. For a scanned head it is
not something an artist should paint: the ear is thin because the GEOMETRY is
thin there, and the geometry is already in the file. So this script measures it.

WHAT "THICKNESS" MEANS HERE. For every texel the mesh covers, the surface point
and its interpolated normal are recovered from the UV layout, and a cone of rays
is cast INTO the surface (around -N). The texel's thickness is the MEDIAN hit
distance of those rays. The median, not the mean or the minimum:

  * a single ray slipping through a crack in a photogrammetry mesh would drag a
    mean to "very thick", and a single ray grazing a neighbouring fold would
    drag a minimum to "paper thin";
  * a cone and not a single ray, because at an ear's rim the surface normal is
    the least reliable thing about the scan.

A texel where most rays MISS (an open surface — the bust is cut off at the
shoulders) is written as THICK, not thin. That is the conservative reading
docs/guides/skin-transmission.md asks for: "a missing thickness transmits
nothing, not everything". A texel the UV layout does not cover is also thick,
and covered texels are dilated outward over it so bilinear filtering and mips
never blend an ear's thin value with the empty atlas around it.

ENCODING. value = min(thickness, MaxThickness) / MaxThickness, 8-bit linear.
The material's ThicknessFactor is then MaxThickness in METRES (see --scale),
so the engine's product factor * map recovers the measured thickness.

Needs: numpy, Pillow, ufbx (FBX reader), trimesh + embreex (ray casting).
    python -m venv venv && venv/Scripts/python -m pip install numpy pillow ufbx trimesh embreex

Reproduce the committed map:
    venv/Scripts/python tools/skin-thickness/bake_thickness.py
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MESH = REPO / "OloEditor/assets/models/InfiniteScanHead/Head.fbx"
DEFAULT_OUT = REPO / "OloEditor/SandboxProject/Assets/Textures/InfiniteScanHead_Thickness.png"


def load_fbx_triangles(path: Path):
    """Return (positions[V,3], tri_pos_idx[T,3], tri_uv[T,3,2], tri_nrm[T,3,3])."""
    import ufbx

    scene = ufbx.load_file(str(path))
    if len(scene.meshes) != 1:
        sys.exit(f"expected exactly one mesh in {path}, found {len(scene.meshes)}")
    mesh = scene.meshes[0]
    if not mesh.vertex_uv.exists or not mesh.vertex_normal.exists:
        sys.exit(f"{path}: the mesh needs both UVs and normals")

    positions = np.array([[v.x, v.y, v.z] for v in mesh.vertices], dtype=np.float64)
    vertex_indices = np.array(list(mesh.vertex_indices), dtype=np.int64)

    uv_values = np.array([[u.x, u.y] for u in mesh.vertex_uv.values], dtype=np.float64)
    uv_indices = np.array(list(mesh.vertex_uv.indices), dtype=np.int64)
    n_values = np.array([[n.x, n.y, n.z] for n in mesh.vertex_normal.values], dtype=np.float64)
    n_indices = np.array(list(mesh.vertex_normal.indices), dtype=np.int64)

    corners = []
    for face in mesh.faces:
        # Fan triangulation is exact for the triangles and quads a scan exports;
        # anything larger would need ufbx.triangulate_face, and is refused
        # rather than fanned into a possibly concave polygon.
        if face.num_indices > 4:
            sys.exit(f"{path}: a {face.num_indices}-gon; this script only fans triangles and quads")
        for k in range(1, face.num_indices - 1):
            corners.append((face.index_begin, face.index_begin + k, face.index_begin + k + 1))
    corners = np.array(corners, dtype=np.int64)

    tri_pos_idx = vertex_indices[corners]
    tri_uv = uv_values[uv_indices[corners]]
    tri_nrm = n_values[n_indices[corners]]
    return positions, tri_pos_idx, tri_uv, tri_nrm


def rasterize_uv(tri_uv, resolution):
    """For every texel centre inside a UV triangle: (row, col, triangle, barycentrics).

    Row 0 is the TOP of the image and v runs UP, the convention the head's own
    albedo (Textures/lambertian.jpg) is authored in, so the baked map is sampled
    exactly as that albedo is.
    """
    rows, cols, tris, bary = [], [], [], []
    px = tri_uv[:, :, 0] * resolution - 0.5
    py = (1.0 - tri_uv[:, :, 1]) * resolution - 0.5
    for t in range(len(tri_uv)):
        x0, x1, x2 = px[t]
        y0, y1, y2 = py[t]
        denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(denom) < 1e-12:
            continue
        cmin = max(int(np.floor(min(x0, x1, x2))), 0)
        cmax = min(int(np.ceil(max(x0, x1, x2))), resolution - 1)
        rmin = max(int(np.floor(min(y0, y1, y2))), 0)
        rmax = min(int(np.ceil(max(y0, y1, y2))), resolution - 1)
        if cmin > cmax or rmin > rmax:
            continue
        cc, rr = np.meshgrid(np.arange(cmin, cmax + 1), np.arange(rmin, rmax + 1))
        cc = cc.ravel().astype(np.float64)
        rr = rr.ravel().astype(np.float64)
        w0 = ((y1 - y2) * (cc - x2) + (x2 - x1) * (rr - y2)) / denom
        w1 = ((y2 - y0) * (cc - x2) + (x0 - x2) * (rr - y2)) / denom
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not inside.any():
            continue
        rows.append(rr[inside].astype(np.int64))
        cols.append(cc[inside].astype(np.int64))
        tris.append(np.full(inside.sum(), t, dtype=np.int64))
        bary.append(np.stack([w0[inside], w1[inside], w2[inside]], axis=1))
    return (np.concatenate(rows), np.concatenate(cols), np.concatenate(tris), np.concatenate(bary))


def cone_directions(normals, count, half_angle_deg, rng):
    """`count` directions per normal, stratified over a cone around -normal."""
    inward = -normals
    helper = np.where(np.abs(inward[:, :1]) < 0.9, np.array([[1.0, 0.0, 0.0]]), np.array([[0.0, 1.0, 0.0]]))
    tangent = np.cross(inward, helper)
    tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
    bitangent = np.cross(inward, tangent)

    cos_max = np.cos(np.radians(half_angle_deg))
    directions = []
    for k in range(count):
        # Stratified in (cos theta, phi) with a per-texel jitter: uniform over the
        # cone's solid angle, and no two rays of one texel in the same stratum.
        u = (k + rng.random(len(normals))) / count
        cos_t = 1.0 - u * (1.0 - cos_max)
        sin_t = np.sqrt(np.maximum(0.0, 1.0 - cos_t * cos_t))
        phi = 2.0 * np.pi * ((k * 0.6180339887498949 + rng.random(len(normals))) % 1.0)
        d = (inward * cos_t[:, None] + tangent * (sin_t * np.cos(phi))[:, None]
             + bitangent * (sin_t * np.sin(phi))[:, None])
        directions.append(d)
    return np.stack(directions, axis=1)  # [N, count, 3]


def dilate(values, valid, passes):
    """Grow covered texels outward over uncovered ones, `passes` texels deep."""
    values = values.copy()
    valid = valid.copy()
    for _ in range(passes):
        acc = np.zeros_like(values)
        cnt = np.zeros_like(values)
        for dr, dc in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            shifted_v = np.roll(np.roll(values, dr, 0), dc, 1)
            shifted_m = np.roll(np.roll(valid, dr, 0), dc, 1)
            acc += np.where(shifted_m, shifted_v, 0.0)
            cnt += shifted_m
        grow = (~valid) & (cnt > 0)
        values[grow] = acc[grow] / cnt[grow]
        valid = valid | grow
    return values, valid


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mesh", type=Path, default=DEFAULT_MESH)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--resolution", type=int, default=1024)
    # The scene's own scale: DigitalHuman.olo scales Head.fbx by 0.38, so one
    # mesh unit is 0.38 m in the frame a viewer judges. See the README for why
    # the scene's scale and not an anthropometric one.
    parser.add_argument("--scale", type=float, default=0.38, help="metres per mesh unit")
    parser.add_argument("--max-thickness-mm", type=float, default=20.0)
    parser.add_argument("--rays", type=int, default=16)
    parser.add_argument("--cone-deg", type=float, default=30.0)
    parser.add_argument("--dilate", type=int, default=8)
    # The far-side test: after a ray leaves the surface, cast this many rays
    # in a cone around its continuing direction. A far face that most of them
    # cannot escape from is ENCLOSED, and the ray reads as thick.
    parser.add_argument("--far-rays", type=int, default=8)
    parser.add_argument("--far-cone-deg", type=float, default=60.0)
    parser.add_argument("--far-open-fraction", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=1394)
    args = parser.parse_args()

    import trimesh

    positions, tri_pos_idx, tri_uv, tri_nrm = load_fbx_triangles(args.mesh)
    mesh = trimesh.Trimesh(vertices=positions, faces=tri_pos_idx, process=False)
    intersector = trimesh.ray.ray_pyembree.RayMeshIntersector(mesh)

    res = args.resolution
    rows, cols, tris, bary = rasterize_uv(tri_uv, res)
    tri_p = positions[tri_pos_idx]
    points = np.einsum("nk,nkd->nd", bary, tri_p[tris])
    normals = np.einsum("nk,nkd->nd", bary, tri_nrm[tris])
    normals /= np.linalg.norm(normals, axis=1, keepdims=True)

    # The mesh unit -> millimetres, and an origin offset that clears the ray's
    # own triangle: 0.02 mm, far below any thickness this map can encode.
    mm_per_unit = args.scale * 1000.0
    epsilon = 0.02 / mm_per_unit

    rng = np.random.default_rng(args.seed)
    directions = cone_directions(normals, args.rays, args.cone_deg, rng)
    n_texels = len(points)
    origins = np.repeat(points, args.rays, axis=0) - np.repeat(normals, args.rays, axis=0) * epsilon
    dirs = directions.reshape(-1, 3)

    hit_tri, hit_ray, hit_loc = intersector.intersects_id(origins, dirs, multiple_hits=False,
                                                           return_locations=True)
    distance = np.full(len(origins), np.inf)
    order = np.argsort(hit_ray)
    hit_tri, hit_ray, hit_loc = hit_tri[order], hit_ray[order], hit_loc[order]
    distance[hit_ray] = np.linalg.norm(hit_loc - origins[hit_ray], axis=1) * mm_per_unit
    # THE FAR SIDE MUST BE OPEN. A ray that crosses a thin slab and comes out
    # into a POCKET — the closed eyelid over the scan's eye cavity is the case
    # that forced this — has crossed tissue no backlight can reach from behind:
    # light gets to that far face only by crossing the whole head first. Local
    # thickness alone calls it thin, and the renderer cannot correct that,
    # because the shadow pass culls front faces and so never occludes a closed
    # mesh's own far side. So each crossing is kept only if a cone of rays from
    # its exit point, around the direction it was travelling, mostly escapes.
    crossed = np.nonzero(np.isfinite(distance) & (distance < args.max_thickness_mm))[0]
    if len(crossed) and args.far_rays > 0:
        exit_points = hit_loc[np.searchsorted(hit_ray, crossed)]
        exit_dirs = dirs[crossed]
        far = cone_directions(-exit_dirs, args.far_rays, args.far_cone_deg, rng).reshape(-1, 3)
        far_origins = np.repeat(exit_points + exit_dirs * epsilon, args.far_rays, axis=0)
        _, far_hit_ray = intersector.intersects_id(far_origins, far, multiple_hits=False)
        blocked = np.bincount(far_hit_ray // args.far_rays, minlength=len(crossed))
        open_fraction = 1.0 - blocked / args.far_rays
        enclosed = crossed[open_fraction < args.far_open_fraction]
        distance[enclosed] = np.inf
        print(f"  crossings tested: {len(crossed)}  enclosed (read as thick): {len(enclosed)}")
    distance = distance.reshape(n_texels, args.rays)

    thickness_mm = np.median(distance, axis=1)  # inf when most rays miss -> thick
    capped = np.minimum(thickness_mm, args.max_thickness_mm)

    image = np.full((res, res), args.max_thickness_mm)
    valid = np.zeros((res, res), dtype=bool)
    image[rows, cols] = capped
    valid[rows, cols] = True
    image, _ = dilate(image, valid, args.dilate)

    encoded = np.clip(np.round(image / args.max_thickness_mm * 255.0), 0, 255).astype(np.uint8)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(encoded, mode="L").convert("RGB").save(args.out, optimize=True)

    finite = thickness_mm[np.isfinite(thickness_mm)]
    print(f"wrote {args.out.relative_to(REPO) if args.out.is_relative_to(REPO) else args.out}  {res}x{res}")
    print(f"  texels covered: {n_texels}  rays: {len(origins)}  mostly-missed texels: {n_texels - len(finite)}")
    print(f"  measured thickness (mm): p5 {np.percentile(finite, 5):.2f}  p50 {np.percentile(finite, 50):.2f}  "
          f"p95 {np.percentile(finite, 95):.2f}")
    print(f"  ThicknessFactor to author: {args.max_thickness_mm / 1000.0:g} (metres)")


if __name__ == "__main__":
    main()

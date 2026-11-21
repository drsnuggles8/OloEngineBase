"""Hydraulic and thermal erosion on a metre-valued height grid.

The hydraulic pass is the droplet model the engine's own `TerrainGenerator::ApplyErosion` uses
(inertia, sediment capacity, deposit and erode speeds, evaporation), run in batches of droplets
stepped together so numpy does the work. It adds three things the engine pass has no input for:

  * a HARDNESS map, so a cliff band resists and a dune does not;
  * the SEA: a droplet that reaches the waterline drops its load there and stops, which is what
    builds a fan at the foot of every gully instead of carving the seabed;
  * a FLOW map, the water that passed through each cell, which the material bake reads as
    drainage (darker, wetter, greener in the gullies).
"""
import numpy as np

from .fields import blur


def _bilinear(h, px, pz):
    ix = np.floor(px).astype(np.int64)
    iz = np.floor(pz).astype(np.int64)
    fx = px - ix
    fz = pz - iz
    h00 = h[iz, ix]
    h10 = h[iz, ix + 1]
    h01 = h[iz + 1, ix]
    h11 = h[iz + 1, ix + 1]
    height = (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz
    gx = (h10 - h00) * (1 - fz) + (h11 - h01) * fz
    gz = (h01 - h00) * (1 - fx) + (h11 - h10) * fx
    return height, gx, gz, ix, iz, fx, fz


def _scatter(grid, ix, iz, fx, fz, amount):
    np.add.at(grid, (iz, ix), amount * (1 - fx) * (1 - fz))
    np.add.at(grid, (iz, ix + 1), amount * fx * (1 - fz))
    np.add.at(grid, (iz + 1, ix), amount * (1 - fx) * fz)
    np.add.at(grid, (iz + 1, ix + 1), amount * fx * fz)


def hydraulic(height, cell, seed, droplets, hardness=None, sea_level=0.0, steps=72, batch=20000,
              inertia=0.08, capacity=4.0, min_capacity=0.0004, deposit=0.3, erode=0.3,
              evaporate=0.015, gravity=4.0, spread_cells=1.1, start_mask=None):
    """Erode `height` (metres) in place. Returns (flow, eroded, deposited) maps.

    `hardness` in [0, 1] scales the erode speed down (1 = does not erode). `start_mask` weights
    where rain falls; it defaults to everything above the sea.
    """
    n = height.shape[0]
    rng = np.random.default_rng(seed)
    # The droplet model's constants (capacity, gravity, the capacity floor) are tuned for heights
    # normalised to the map's own width, where one cell step down a 45 deg slope is 1/n. Feeding
    # it metres or cells makes a droplet's speed and load run away within a few steps.
    unit = cell * n
    h = height / unit
    sea = sea_level / unit
    soft = np.ones_like(h) if hardness is None else np.clip(1.0 - hardness, 0.0, 1.0)
    flow = np.zeros_like(h)
    eroded_total = np.zeros_like(h)
    deposited_total = np.zeros_like(h)

    land = (h > sea) if start_mask is None else (start_mask > 0)
    cand_z, cand_x = np.nonzero(land[1:-2, 1:-2])
    if cand_x.size == 0:
        return flow, eroded_total, deposited_total
    weights = None
    if start_mask is not None:
        weights = start_mask[1:-2, 1:-2][cand_z, cand_x].astype(np.float64)
        weights /= weights.sum()

    done = 0
    while done < droplets:
        count = min(batch, droplets - done)
        done += count
        pick = rng.choice(cand_x.size, size=count, p=weights)
        px = cand_x[pick] + 1 + rng.random(count)
        pz = cand_z[pick] + 1 + rng.random(count)
        dx = np.zeros(count)
        dz = np.zeros(count)
        speed = np.ones(count)
        water = np.ones(count)
        sediment = np.zeros(count)
        alive = np.ones(count, dtype=bool)
        before = h.copy()
        flat_index_scale = n

        for _ in range(steps):
            if not alive.any():
                break
            idx = np.nonzero(alive)[0]
            x, z = px[idx], pz[idx]
            hh, gx, gz, ix, iz, fx, fz = _bilinear(h, x, z)
            ndx = dx[idx] * inertia - gx * (1 - inertia)
            ndz = dz[idx] * inertia - gz * (1 - inertia)
            length = np.hypot(ndx, ndz)
            flat = length < 1e-12
            if flat.any():
                ang = rng.random(int(flat.sum())) * 2 * np.pi
                ndx[flat] = np.cos(ang)
                ndz[flat] = np.sin(ang)
                length[flat] = 1.0
            ndx /= length
            ndz /= length
            nx_ = x + ndx
            nz_ = z + ndz
            inside = (nx_ >= 1) & (nx_ < n - 2) & (nz_ >= 1) & (nz_ < n - 2)
            nx_c = np.clip(nx_, 1, n - 2.001)
            nz_c = np.clip(nz_, 1, n - 2.001)
            h_new = _bilinear(h, nx_c, nz_c)[0]
            dh = h_new - hh

            cellid = iz * flat_index_scale + ix
            np.add.at(flow, (iz, ix), water[idx])
            # Droplets stepped together do not see each other's work, so N of them in one cell
            # would each fill the same pit or cut the same step. They share the change instead:
            # a crowd in a cell acts as one droplet.
            crowd = np.bincount(cellid, minlength=n * n)[cellid].astype(np.float64)

            cap = np.maximum(-dh * speed[idx] * water[idx] * capacity, min_capacity)
            sed = sediment[idx]
            # Reaching the sea ends the droplet: everything it carries settles at the waterline.
            at_sea = hh <= sea
            dropping = (sed > cap) | (dh > 0) | at_sea
            dep = np.where(at_sea, sed, np.where(dh > 0, np.minimum(dh, sed), (sed - cap) * deposit))
            dep = np.where(dropping, np.maximum(dep, 0.0), 0.0)
            ero = np.where(dropping, 0.0, np.minimum((cap - sed) * erode, -dh))
            ero = np.maximum(ero, 0.0) * soft[iz, ix]
            _scatter(h, ix, iz, fx, fz, (dep - ero) / crowd)
            _scatter(deposited_total, ix, iz, fx, fz, dep / crowd)
            _scatter(eroded_total, ix, iz, fx, fz, ero / crowd)
            sediment[idx] = sed - dep + ero

            speed[idx] = np.sqrt(np.maximum(speed[idx] ** 2 - dh * gravity, 0.0))
            water[idx] *= (1 - evaporate)
            dx[idx] = ndx
            dz[idx] = ndz
            px[idx] = nx_c
            pz[idx] = nz_c
            alive[idx] = inside & ~at_sea & (water[idx] > 0.01)

        # Spread each batch's change over a small footprint: a one-cell pit is the droplet
        # model's signature artefact, and this is what the engine pass's brush radius does.
        h = before + blur(h - before, spread_cells)

    height[:] = h * unit
    return flow, eroded_total * unit, deposited_total * unit


def thermal(height, cell, talus_deg, iterations=40, rate=0.35, mask=None):
    """Relax slopes steeper than `talus_deg` (a scalar or a per-cell map). Returns the deposit map.

    Material leaves a cell toward each lower 4-neighbour whose slope exceeds the angle of repose.
    Where `talus_deg` is high (rock) the face stands; where it is low (sand, scree) it slumps.
    """
    talus = np.tan(np.radians(talus_deg)) * cell
    if np.ndim(talus) == 0:
        talus = np.full_like(height, float(talus))
    deposited = np.zeros_like(height)
    h = height
    for _ in range(iterations):
        moved = np.zeros_like(h)
        for axis, shift in ((0, 1), (0, -1), (1, 1), (1, -1)):
            neighbour = np.roll(h, shift, axis=axis)
            diff = h - neighbour
            excess = np.maximum(diff - talus, 0.0) * (rate * 0.25)
            if mask is not None:
                excess = excess * mask
            # No wrap-around across the tile edge.
            if axis == 0:
                if shift == 1:
                    excess[0, :] = 0
                else:
                    excess[-1, :] = 0
            else:
                if shift == 1:
                    excess[:, 0] = 0
                else:
                    excess[:, -1] = 0
            moved -= excess
            gain = np.roll(excess, -shift, axis=axis)
            moved += gain
            deposited += gain
        h += moved
    return deposited

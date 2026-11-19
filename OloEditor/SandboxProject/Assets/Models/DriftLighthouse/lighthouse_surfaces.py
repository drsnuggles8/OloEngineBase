"""Bake surface finishes for Stacks Light from the bundled CC0 scan channels.

All colour arithmetic below uses display RGB. Normals and the packed occlusion,
roughness and metallic channels are data and are written without a transfer curve.
"""

import tempfile
import zipfile
from pathlib import Path

import bpy
import numpy as np

# The ogee above the drip edge: elevations and radii shared by the sheet
# pattern and its geometry. The UV's longitudinal coordinate follows the metal.
HOOD = ((21.66, 1.88), (22.14, 1.50), (22.52, 0.91), (22.74, 0.33), (22.89, 0.22))
HOOD_GUTTER_FRACTION = 1 / 64
HOOD_SLANT = np.r_[0, np.cumsum(np.linalg.norm(np.diff(HOOD, axis=0), axis=1))]
HOOD_V = HOOD_SLANT / HOOD_SLANT[-1]


def smooth(low, high, x):
    t = np.clip((x - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def mix(a, b, fraction):
    t = fraction[..., None]
    return a * (1 - t) + b * t


def luma(rgb):
    return rgb[..., 0] * 0.30 + rgb[..., 1] * 0.59 + rgb[..., 2] * 0.11


def noise(size, nx, ny, seed):
    """Periodic interpolated noise. Changing aspect stretches the features in metres."""
    rng = np.random.default_rng(seed)
    grid = rng.random((ny, nx), dtype=np.float32)
    x = np.arange(size, dtype=np.float32)[None, :] * (nx / size)
    y = np.arange(size, dtype=np.float32)[:, None] * (ny / size)
    ix, iy = x.astype(int), y.astype(int)
    fx, fy = smooth(0, 1, x - ix), smooth(0, 1, y - iy)
    a = grid[iy, ix] * (1 - fx) + grid[iy, (ix + 1) % nx] * fx
    b = grid[(iy + 1) % ny, ix] * (1 - fx) + grid[(iy + 1) % ny, (ix + 1) % nx] * fx
    return (a * (1 - fy) + b * fy).astype(np.float32)


def response(occlusion, roughness, metallic):
    return np.stack(np.broadcast_arrays(occlusion, roughness, metallic), axis=-1)


def relief(normal, strength):
    v = normal * 2 - 1
    v[..., :2] *= np.asarray(strength)[..., None]
    v /= np.maximum(0.001, np.linalg.norm(v, axis=-1, keepdims=True))
    return v * 0.5 + 0.5


def read_scan(folder, name):
    def channel(key):
        img = bpy.data.images.load(str(folder / name / (key + ".jpg")))
        img.colorspace_settings.name = "Non-Color"
        w, h = img.size
        buf = np.empty(w * h * 4, dtype=np.float32)
        img.pixels.foreach_get(buf)
        bpy.data.images.remove(img)
        return buf.reshape(h, w, 4)[:, :, :3].copy()

    return channel("Diffuse"), channel("nor_gl"), channel("arm")


class Surfaces:
    def __init__(self, destination):
        self.destination = destination
        self.images = {}
        self.finishes = {}

    def image(self, name, rgb, colour=False):
        h, w, _ = rgb.shape
        if name.endswith("ORM"):
            rgb = rgb.reshape(h // 2, 2, w // 2, 2, 3).mean(axis=(1, 3))
            h, w, _ = rgb.shape
        img = bpy.data.images.new(name, w, h, alpha=False)
        img.colorspace_settings.name = "Non-Color"
        buf = np.ones((h, w, 4), np.float32)
        buf[..., :3] = np.clip(rgb, 0, 1)
        img.pixels.foreach_set(buf.ravel())
        img.file_format = "PNG"
        img.filepath_raw = str(self.destination / (name + ".png"))
        img.save()
        if colour:
            img.colorspace_settings.name = "sRGB"
        self.images[name] = img

    def finish(self, name, colour, normal, orm):
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        tree = mat.node_tree
        shader = tree.nodes["Principled BSDF"]
        for key, target in (
            (colour, "Base Color"),
            (normal, "Normal"),
            (orm, "Roughness"),
        ):
            tex = tree.nodes.new("ShaderNodeTexImage")
            tex.image = self.images[key]
            if target == "Normal":
                bump = tree.nodes.new("ShaderNodeNormalMap")
                tree.links.new(tex.outputs["Color"], bump.inputs["Color"])
                tree.links.new(bump.outputs["Normal"], shader.inputs[target])
            elif target == "Roughness":
                split = tree.nodes.new("ShaderNodeSeparateColor")
                tree.links.new(tex.outputs["Color"], split.inputs["Color"])
                tree.links.new(split.outputs["Green"], shader.inputs["Roughness"])
                tree.links.new(split.outputs["Blue"], shader.inputs["Metallic"])
            else:
                tree.links.new(tex.outputs["Color"], shader.inputs[target])
        self.finishes[name] = (colour, normal, orm)
        return mat

    def walls(self, source):
        diffuse, normal, arm = read_scan(source, "sandstone_blocks_05")
        lum = luma(diffuse)
        size = len(lum)
        ao, rough = arm[..., 0], arm[..., 1]
        self.bed_joint_v = float(np.argmin(ao.mean(axis=1))) / size
        stone = mix(diffuse, np.array([0.49, 0.46, 0.405]), np.full_like(lum, 0.33))
        # Thin lime slurry settles into the cut faces. Wear breaks through on
        # ridges; deep recessed joints keep their cool, dirtier mortar colour.
        patch = (
            0.48 * noise(size, 8, 11, 91)
            + 0.34 * noise(size, 37, 42, 19)
            + 0.18 * noise(size, 151, 160, 82)
        )
        spall = smooth(0.665, 0.79, patch + 0.14 * (1 - normal[..., 2]))
        joint = smooth(0.26, 0.70, 1 - ao + 0.48 * (0.55 - lum))
        grain = lum - np.median(lum)
        tone = 0.98 + 0.34 * grain + 0.10 * (patch - 0.5)
        for name, pigment, loss in (
            ("StacksMasonry", [0.84, 0.819, 0.751], 0.76),
            ("StacksOxide", [0.62, 0.221, 0.141], 0.83),
            ("StacksPlinth", [0.43, 0.416, 0.373], 0.42),
        ):
            colour = mix(np.array(pigment) * tone[..., None], stone, spall * loss)
            colour = mix(
                colour,
                np.array([0.44, 0.413, 0.36]) * (0.8 + lum[..., None] * 0.3),
                joint * 0.88,
            )
            self.image(name, colour, True)
            if name == "StacksMasonry":
                v, u = np.indices((size, size), dtype=np.float32) / size
                # At the top of the wall this tile covers z=15..18 m. Two
                # iron gallery knees drain onto each repeat of the facing.
                stain = np.zeros_like(v)
                for center, length in ((0.0, 2.14), (0.5, 1.28)):
                    drop = 17.32 - (15 + 3 * ((v - self.bed_joint_v) % 1))
                    dist = abs((u - center + 0.009 * np.sin(v * 9) + 0.5) % 1 - 0.5)
                    width = 0.009 + 0.038 * np.clip(1 - drop / length, 0, 1)
                    stain += (
                        np.exp(-((dist / width) ** 2))
                        * smooth(-0.1, 0.12, drop)
                        * (1 - smooth(0.15, length, drop))
                    )
                self.image(
                    "StacksRunoff",
                    mix(
                        colour,
                        np.array([0.44, 0.32, 0.205]),
                        stain * (0.38 + 0.27 * patch),
                    ),
                    True,
                )
        self.image(
            "StacksMasonryNormal", relief(normal, 0.38 + 0.50 * spall + 0.38 * joint)
        )
        self.image(
            "StacksMasonryORM", response(0.43 + ao * 0.57, 0.63 + rough * 0.32, 0)
        )

    def granite(self, source):
        diffuse, normal, arm = read_scan(source, "granite_tile")
        lum = luma(diffuse)
        size = len(lum)
        # A split face is blue-grey in the spray zone. Dressed sandstone
        # trimmings are warmer and show diagonal bolster marks in raking light.
        v, u = np.indices((size, size), dtype=np.float32) / size
        weather = noise(size, 6, 9, 86)
        fleck = 0.98 + 2.1 * (lum - np.median(lum)) + 0.22 * (weather - 0.5)
        salt = smooth(0.63, 0.93, 0.65 * weather + 0.35 * noise(size, 33, 38, 55))
        self.image(
            "StacksBasalt",
            mix(
                np.array([0.435, 0.428, 0.393]) * fleck[..., None],
                np.array([0.65, 0.65, 0.585]),
                salt * 0.26,
            ),
            True,
        )
        tool = np.sin((u * 83 + v * 39 + weather * 1.3) * np.pi * 2) * smooth(
            0.43, 0.69, weather
        )
        self.image(
            "StacksFreestone",
            np.array([0.655, 0.595, 0.494]) * (fleck + 0.035 * tool)[..., None],
            True,
        )
        self.image("StacksGraniteNormal", relief(normal, 1.18))
        self.image(
            "StacksGraniteORM",
            response(0.5 + 0.5 * arm[..., 0], 0.56 + 0.38 * arm[..., 1], 0),
        )

        # The nosings polish where boots land. Under the rails the dressed
        # face remains coarse, with a damp olive cast around the post feet.
        wear = (1 - smooth(0.16, 0.44, abs(u - 0.5))) * (0.70 + 0.30 * weather)
        sheltered = smooth(0.30, 0.47, abs(u - 0.5)) * smooth(0.30, 0.78, weather)
        tread = np.array([0.62, 0.585, 0.505]) * (fleck + tool * 0.028)[..., None]
        tread = mix(tread, tread * np.array([0.84, 0.86, 0.90]), wear)
        self.image(
            "StacksTreads",
            mix(tread, np.array([0.37, 0.387, 0.299]), sheltered * 0.56),
            True,
        )
        self.image(
            "StacksTreadsORM",
            response(
                0.5 + 0.5 * arm[..., 0], 0.82 - 0.37 * wear + 0.05 * (weather - 0.5), 0
            ),
        )

    def oak(self, source):
        diffuse, normal, arm = read_scan(source, "weathered_brown_planks")
        lum = luma(diffuse)
        # Tar in the softer summer grain leaves the raised fibres warm.
        tar = np.clip(0.36 - 0.65 * (lum - 0.36), 0.1, 0.68)
        self.image(
            "StacksOak",
            mix(
                diffuse * np.array([1.17, 1.02, 0.82]),
                np.array([0.162, 0.125, 0.091]),
                tar,
            ),
            True,
        )
        self.image("StacksOakNormal", relief(normal, 0.75))
        self.image(
            "StacksOakORM",
            response(0.42 + 0.58 * arm[..., 0], 0.37 + 0.55 * arm[..., 1], 0),
        )

    def metals(self, source):
        diffuse, normal, arm = read_scan(source, "rusty_metal_03")
        size = len(diffuse)
        lum = luma(diffuse)
        pits = smooth(0.23, 0.48, 0.64 - lum)
        broad = noise(size, 5, 7, 1874)
        runoff = noise(size, 39, 5, 271)
        scale = (
            0.48 * broad + 0.22 * runoff + 0.2 * noise(size, 32, 28, 23) + 0.1 * pits
        )
        # Most of the exposed roof has an intact oxide layer. Copper shows
        # through in small sheltered islands, plus the folded eaves and seams.
        oxide = smooth(0.22, 0.58, scale)
        green = np.array([0.346, 0.468, 0.413])
        copper = mix(np.array([0.377, 0.361, 0.278]), green, oxide)
        powder = smooth(0.41, 0.69, scale) * 0.43
        copper = mix(copper, np.array([0.54, 0.615, 0.53]), powder)
        self.image("StacksCopper", copper * (0.95 + 0.13 * lum[..., None]), True)
        self.image(
            "StacksCopperORM",
            response(
                0.80 + 0.20 * arm[..., 0], 0.53 + 0.16 * oxide, 0.38 - 0.25 * oxide
            ),
        )
        chip = smooth(0.71, 0.88, 0.65 * pits + 0.35 * broad)
        iron = mix(
            np.array([0.176, 0.214, 0.212]) * (0.93 + 0.17 * lum[..., None]),
            np.array([0.40, 0.257, 0.159]),
            chip * 0.73,
        )
        self.image("StacksEnamel", iron, True)
        self.image(
            "StacksEnamelORM",
            response(
                0.77 + 0.23 * arm[..., 0],
                0.39 + 0.34 * chip + 0.10 * broad,
                0.12 + 0.13 * chip,
            ),
        )
        bronze = mix(
            np.array([0.65, 0.487, 0.291]),
            np.array([0.43, 0.395, 0.271]),
            smooth(0.37, 0.70, scale),
        )
        self.image("StacksBronze", bronze * (0.95 + 0.16 * lum[..., None]), True)
        self.image(
            "StacksBronzeORM",
            response(
                0.82 + 0.18 * arm[..., 0], 0.30 + 0.19 * oxide, 0.73 - 0.19 * oxide
            ),
        )
        self.image("StacksMetalNormal", relief(normal, 0.19))

        self.deck_plate(iron, arm)
        self.hood_panels(lum, arm)

    def deck_plate(self, iron, arm):
        size = len(iron)
        # Alternating diagonals of cast lozenges. A shallow
        # rounded rib has a polished crown above the darker painted valleys.
        y, x = np.indices((size, size), dtype=np.float32) / size * 6
        sx, sy = (x % 1) - 0.5, (y % 1) - 0.5
        direction = 1 - 2 * ((x.astype(int) + y.astype(int)) % 2)
        length, across = (sx + direction * sy) * 0.7071, (sy - direction * sx) * 0.7071
        profile = 1 - smooth(0.63, 1.0, abs(length) / 0.47 + abs(across) / 0.11)
        h = 0.0025 * profile
        nx = (np.roll(h, 1, 1) - np.roll(h, -1, 1)) * size / 1.08
        ny = (np.roll(h, 1, 0) - np.roll(h, -1, 0)) * size / 1.08
        n = np.stack([nx, ny, np.ones_like(nx)], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self.image("StacksDeckNormal", n * 0.5 + 0.5)
        self.image(
            "StacksDeck",
            mix(iron, np.array([0.39, 0.389, 0.353]), profile * 0.74),
            True,
        )
        self.image(
            "StacksDeckORM",
            response(
                0.86 + 0.14 * arm[..., 0], 0.71 - 0.32 * profile, 0.25 + 0.38 * profile
            ),
        )

    def hood_panels(self, lum, arm):
        size = len(lum)
        # Four flattened gores. Their centre fields have a thin, even oxide;
        # water follows the two upstands and accumulates along the lower hem.
        v, u = np.indices((size, size), dtype=np.float32) / (size - 1)
        half_width = np.interp(v, HOOD_V, [r for _, r in HOOD]) / (2 * HOOD[0][1])
        distance = abs(half_width - abs(u - 0.5))
        colours, responses = [], []
        for panel in range(4):
            cloud = 0.6 * noise(size, 5, 6, 910 + panel) + 0.4 * noise(
                size, 23, 15, 325 + panel
            )
            tracks = noise(size, 83, 7, 180 + panel)
            seam = (1 - smooth(0.006, 0.088, distance)) * (0.44 + 0.56 * tracks)
            hem = (1 - smooth(0.008, 0.16, v)) * (0.29 + 0.71 * tracks)
            seam_oxide = np.clip(seam + hem, 0, 1)
            skin = np.array([0.365, 0.474, 0.422]) + (
                cloud[..., None] - 0.5
            ) * np.array([0.105, 0.135, 0.109])
            skin = mix(skin, np.array([0.553, 0.625, 0.515]), seam_oxide * 0.70)
            folded = (1 - smooth(0.001, 0.010, distance)) * (0.70 + 0.30 * tracks)
            skin = mix(skin, np.array([0.373, 0.30, 0.199]), folded)
            colours.append(skin * (0.979 + 0.057 * lum[..., None]))
            responses.append(
                response(
                    0.90 + 0.10 * arm[..., 0],
                    0.58 + 0.13 * cloud - 0.21 * folded,
                    0.13 + 0.44 * folded,
                )
            )

        def pack(panels):
            # Duplicate texels outside each panel so bilinear/mip filtering
            # samples its own folded edge or hem on both sides of its boundary.
            gutter = round(size * HOOD_GUTTER_FRACTION)
            index = np.rint(np.linspace(0, size - 1, size - 2 * gutter)).astype(int)
            padded = [
                np.pad(
                    panel[np.ix_(index, index)],
                    ((gutter, gutter), (gutter, gutter), (0, 0)),
                    mode="edge",
                )
                for panel in panels
            ]
            return np.vstack([np.hstack(padded[:2]), np.hstack(padded[2:])])

        self.image("StacksHood", pack(colours), True)
        self.image("StacksHoodORM", pack(responses))

    def build(self, archive):
        with tempfile.TemporaryDirectory(prefix="stacks-scans-") as tmp:
            with zipfile.ZipFile(archive) as inputs:
                for scan in (
                    "sandstone_blocks_05",
                    "granite_tile",
                    "weathered_brown_planks",
                    "rusty_metal_03",
                ):
                    directory = Path(tmp) / scan
                    directory.mkdir()
                    for channel in ("Diffuse", "nor_gl", "arm"):
                        (directory / (channel + ".jpg")).write_bytes(
                            inputs.read(f"{scan}/{channel}.jpg")
                        )
            for bake in (self.walls, self.granite, self.oak, self.metals):
                bake(Path(tmp))

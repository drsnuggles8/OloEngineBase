"""Writes the six island entities into Scenes/Drift.olo.

The scene block of an island is derived data: the tile size and height scale come from
recipes.TILES, the layer tiling from the tile size and each layer's repeat in metres, and the
landfall radius from the generated height field. So the tool stamps the whole entity, from its
header comment to the end of its FoliageComponent, and nothing in that range is edited by hand.
"""
import io
import math
import os

from . import layers, recipes
from .materials import LAYERS, NEUTRAL

ASSET_DIR = "Assets/Terrain/DriftIslands"
VEG = "Assets/Models/Vegetation"
SEA_ALTITUDE = 24.0  # foliage altitude bands are metres above the TILE FLOOR, which is 24 m down

ENTITY_ID = {"Ridgeback": 879000000000005, "Mesa": 879000000000006, "Atoll": 879000000000007,
             "Stacks": 879000000000008, "Dunes": 879000000000009, "Sisters": 879000000000011}

CHARACTER = {
    "Ridgeback": [
        "The steering target, dead ahead of the start line. A whaleback of gneiss: a long spine",
        "with a summit and a saddle, sheer on its weather (north) side, grass falling south to a",
        "rock shore with shingle coves. The timber landing and hand crane stand on a low rock",
        "spit in the lee of the west point.",
    ],
    "Mesa": [
        "A table of bedded red sandstone: a caprock rim, a bench where a softer bed has",
        "weathered back, a second wall, and a talus apron down to a narrow red shore. The one",
        "island that reads by its PROFILE even as a smudge on the horizon.",
    ],
    "Atoll": [
        "A ring reef awash with four sand cays on it, a pale lagoon inside and one pass",
        "through the reef, facing the start line. Six metres at its highest: from the sea it is",
        "a line of white sand and a canopy.",
    ],
    "Stacks": [
        "A block of black basalt with a tilted grass top, an outlier beside it and sea stacks",
        "on a wave-cut platform. Faces step back in tiers, one per lava flow. Stacks Light",
        "stands on the outer western stack.",
    ],
    "Dunes": [
        "Sand on a low platform: a foredune behind the windward beach, crescent dunes with",
        "their slip faces downwind, machair in their lee, and a spit hooking round a shallow",
        "lagoon. The anti-Stacks: nothing here is rock.",
    ],
    "Sisters": [
        "Two steep islets of the same rock, joined by a shingle bar the swell washes over.",
        "The long leg: about 950 m from the start line, approached from the south.",
    ],
}

# Foliage layers per island. Heights are real metres (the meshes are authored at unit height).
# Channels: 3 = Turf, 4 = Heath (materials.LAYERS).


def _layer(name, species, card, channel, density, height, **kw):
    d = {
        "Name": name, "MeshPath": f"{VEG}/{species}/{species}.obj",
        "AlbedoPath": f"{VEG}/{species}/Textures/{card}", "Density": density, "SplatmapChannel": channel,
        "MinSlopeAngle": 0, "MaxSlopeAngle": 38, "MinScale": 0.85, "MaxScale": 1.2,
        "MinHeight": height[0], "MaxHeight": height[1], "RandomRotation": True, "SlopeFeather": 6,
        # Nothing grows in the swash: the band starts above the storm berm.
        "UseAltitudeBand": True, "MinAltitude": SEA_ALTITUDE + 2.4, "MaxAltitude": 1000, "AltitudeFeather": 0.8,
        "ClumpStrength": 0.5, "ClumpScale": 9, "ClumpFalloff": 1.2, "ClumpScaleInfluence": 0.3,
        "SlopeSinkFactor": 0.6, "ViewDistance": 110, "FadeStartDistance": 85,
        "MeshViewDistance": 16, "MeshFadeStartDistance": 11, "WindStrength": 0.45, "WindSpeed": 1.4,
        "BaseColor": [0.92, 0.95, 0.88], "Roughness": 0.75, "AlphaCutoff": 0.45,
        "TransmissionStrength": 0.45, "LodTransitionSpread": 5, "LodHysteresis": 0.08,
        "LodStochasticCoverage": True, "UseDensityLod": True, "DensityLodStartDistance": 22,
        "DensityLodEndDistance": 80, "DensityLodMinFraction": 0.3, "Enabled": True, "CastShadows": False,
    }
    d.update(kw)
    return d


def _tree(name, species, card, channel, density, height, **kw):
    settings = dict(MaxSlopeAngle=30, MinScale=0.9, MaxScale=1.25, ClumpStrength=0.85, ClumpScale=48,
                    ClumpFalloff=1.6, ClumpScaleInfluence=0.2, ViewDistance=1400, FadeStartDistance=1250,
                    MeshViewDistance=45, MeshFadeStartDistance=32, WindStrength=0.16, WindSpeed=0.8,
                    BaseColor=[0.95, 0.95, 0.92], TransmissionStrength=0.3, UseDensityLod=False,
                    LodStochasticCoverage=True, CastShadows=True, UseImpostor=True, ImpostorStartDistance=45,
                    ImpostorTransitionBand=20, ImpostorFramesPerAxis=8, ImpostorAtlasResolution=2048,
                    ImpostorHemiOctahedral=True)
    settings.update(kw)
    return _layer(name, species, card, channel, density, height, **settings)


FOLIAGE = {
    "Ridgeback": [
        _layer("Turf", "grass", "grass_card.png", 3, 6.0, (0.28, 0.5)),
        _layer("Moor grass", "grass_dry", "grass_dry_card.png", 4, 4.0, (0.3, 0.6), ClumpStrength=0.7, ClumpScale=6,
               BaseColor=[0.95, 0.9, 0.78]),
        _layer("Bracken", "fern", "fern_card.png", 3, 0.5, (0.5, 0.9), UseMoisture=True, MinMoisture=0.55, MaxMoisture=1.0,
               MoistureFeather=0.12, ClumpStrength=0.85, ClumpScale=18, ViewDistance=160, FadeStartDistance=130),
        _layer("Gorse", "shrub", "shrub_card.png", 4, 0.035, (0.8, 1.4), ClumpStrength=0.8, ClumpScale=26,
               ViewDistance=420, FadeStartDistance=360, MeshViewDistance=30, MeshFadeStartDistance=22, CastShadows=True),
        _tree("Wind-cut pines", "pine", "pine_card.png", 3, 0.0035, (5.5, 9.0), UseMoisture=True, MinMoisture=0.5,
              MaxMoisture=1.0, MoistureFeather=0.1, MaxAltitude=SEA_ALTITUDE + 34.0, AltitudeFeather=4.0),
    ],
    "Sisters": [
        _layer("Turf", "grass", "grass_card.png", 3, 6.0, (0.22, 0.42)),
        _layer("Thrift", "flower", "flower_card.png", 3, 0.7, (0.18, 0.3), ClumpStrength=0.9, ClumpScale=7,
               BaseColor=[1.0, 0.86, 0.9], ViewDistance=90, FadeStartDistance=70),
        _layer("Moor grass", "grass_dry", "grass_dry_card.png", 4, 3.5, (0.25, 0.5), BaseColor=[0.95, 0.9, 0.78]),
    ],
    "Stacks": [
        _layer("Turf", "grass", "grass_card.png", 3, 6.0, (0.25, 0.48)),
        _layer("Sea campion", "flower", "flower_card.png", 4, 0.5, (0.16, 0.28), ClumpStrength=0.9, ClumpScale=8,
               BaseColor=[1.0, 1.0, 0.96]),
        _layer("Moor grass", "grass_dry", "grass_dry_card.png", 4, 4.0, (0.25, 0.5)),
    ],
    "Mesa": [
        _layer("Dry grass", "grass_dry", "grass_dry_card.png", 3, 3.0, (0.3, 0.6), ClumpStrength=0.75, ClumpScale=7,
               BaseColor=[1.0, 0.95, 0.78]),
        _layer("Bunch grass", "grass_dry", "grass_dry_card.png", 4, 1.6, (0.4, 0.75), ClumpStrength=0.85, ClumpScale=5,
               BaseColor=[1.0, 0.92, 0.72]),
        _layer("Scrub", "shrub", "shrub_card.png", 4, 0.06, (0.7, 1.3), ClumpStrength=0.8, ClumpScale=22,
               ViewDistance=420, FadeStartDistance=360, MeshViewDistance=30, MeshFadeStartDistance=22, CastShadows=True),
        _tree("Table oaks", "broadleaf", "broadleaf_card.png", 3, 0.0022, (4.5, 7.0)),
    ],
    "Dunes": [
        _layer("Marram", "grass_dry", "grass_dry_card.png", 4, 3.2, (0.55, 0.95), ClumpStrength=0.8, ClumpScale=4.5,
               MaxSlopeAngle=34, BaseColor=[0.96, 0.98, 0.82], WindStrength=0.7),
        _layer("Machair grass", "grass", "grass_card.png", 3, 7.0, (0.2, 0.38)),
        _layer("Machair flowers", "flower", "flower_card.png", 3, 1.6, (0.16, 0.3), ClumpStrength=0.85, ClumpScale=11,
               BaseColor=[1.0, 0.98, 0.9]),
    ],
    "Atoll": [
        _tree("Canopy", "broadleaf", "broadleaf_card.png", 4, 0.02, (6.5, 10.5), ClumpStrength=0.45, ClumpScale=30,
              MinAltitude=SEA_ALTITUDE + 1.9),
        _layer("Understorey", "shrub", "shrub_card.png", 4, 0.22, (0.8, 1.6), ClumpStrength=0.6, ClumpScale=14,
               MinAltitude=SEA_ALTITUDE + 1.7, ViewDistance=420, FadeStartDistance=360, MeshViewDistance=30,
               MeshFadeStartDistance=22, CastShadows=True),
        _layer("Beach grass", "grass_dry", "grass_dry_card.png", 3, 2.5, (0.3, 0.55), MinAltitude=SEA_ALTITUDE + 1.5),
    ],
}


def _fmt(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_fmt(c) for c in v) + "]"
    if isinstance(v, float):
        return f"{v:.6g}"
    return str(v)


def _land_metres(h, size):
    """Tile-local (x, z) in metres of every land sample, by the generator's own mapping
    (fields.xz): sample i is at i * size / (n - 1), which is where the engine draws it and
    collides with it. Every measurement the stamper takes goes through here, so the landfall
    ring, the discovery box and the header's distances describe one coast."""
    import numpy as np
    from . import fields
    n = h.shape[0]
    zz, xx = np.nonzero(h > 0.0)
    if xx.size == 0:
        raise ValueError("the height map has no land above the sea")
    return xx * fields.cell(n, size), zz * fields.cell(n, size)


def enclosing_radius(h, size):
    """Metres from the tile centre to the island's farthest land: its landfall circle.

    The 99.5th percentile of the land's distances, so one awash rock does not set the ring.
    """
    import numpy as np
    xx, zz = _land_metres(h, size)
    return float(np.percentile(np.hypot(xx - size * 0.5, zz - size * 0.5), 99.5))


def land_box(h, size, margin=10.0):
    """(centre x, centre z, half x, half z) of the land's bounding box in tile-local metres,
    grown by `margin`: the discovery trigger. A box sized to the landfall CIRCLE reaches a
    hundred metres off the narrow side of a long island; the land's own extent does not."""
    xx, zz = _land_metres(h, size)
    x0, x1 = xx.min() - margin, xx.max() + margin
    z0, z1 = zz.min() - margin, zz.max() + margin
    return (round((x0 + x1) * 0.5), round((z0 + z1) * 0.5), math.ceil((x1 - x0) * 0.5), math.ceil((z1 - z0) * 0.5))


def island_block(name, radius, box):
    size, height_scale, _, corner_x, corner_z = recipes.TILES[name]
    # DriftWeatherDirector.lua: coast = (IslandFalloffRadius + 0.5) * 0.5 * WorldSizeX.
    falloff = round(2.0 * radius / size - 0.5, 3)
    if not 0.0 <= falloff < 0.5:
        raise ValueError(f"{name}: landfall radius {radius:.1f} m does not fit the weather director's formula")
    box_x, box_z, half_x, half_z = box
    centre = size * 0.5
    o = []
    title = f"  # ── Island - {name} "
    o.append(title + "─" * max(4, 78 - len(title)))
    o += [f"  # {line}" for line in CHARACTER[name]]
    o += [
        "  #",
        "  # GENERATED: tools/drift-islands/build_islands.py --stamp-scene writes this entity, from",
        "  # this header to the end of its FoliageComponent. Change the tool, not this text.",
        "  #",
        "  # The ground is a height MAP (issue #1721), not a noise recipe: the tool models the",
        "  # landform, erodes it, shapes its shore and bakes the material maps beside it. The tile",
        f"  # floor is 24 m under the sea ({24.0 / height_scale:.3f} of the {height_scale:g} m HeightScale) and every border",
        "  # texel of the map is exactly that floor, so the tile edge is seabed, never a wall.",
        f"  - Entity: {ENTITY_ID[name]}",
        "    TagComponent:",
        f"      Tag: Island - {name}",
        "    TransformComponent:",
        f"      # Origin is a CORNER, not the centre: a {size:g} m tile centred on ({corner_x + centre:g}, ., {corner_z + centre:g}).",
        f"      Translation: [{corner_x:g}, -24, {corner_z:g}]",
        "      Rotation: [0, 0, 0]",
        "      Scale: [1, 1, 1]",
        "    TerrainComponent:",
        f"      HeightmapPath: {ASSET_DIR}/{name}_Height.png",
        f"      WorldSizeX: {size:g}",
        f"      WorldSizeZ: {size:g}",
        f"      HeightScale: {height_scale:g}",
        "      CollisionEnabled: true",
        "      ProceduralEnabled: false",
        "      # No mask is applied to a height map. The radius is kept because the weather director",
        "      # derives this island's landfall and departure rings from it:",
        "      #   coast = (IslandFalloffRadius + 0.5) * 0.5 * WorldSizeX",
        f"      # which gives {radius:.0f} m here, the distance from the tile centre to its farthest land.",
        "      ShapingIslandFalloff: 0",
        f"      ShapingIslandFalloffRadius: {falloff:g}",
        "      AutoMaterial: false",
        "      # The GPU LOD quadtree (issue #714) drives this terrain; nodes are picked by",
        "      # screen-space error, so the island resolves as the boat closes instead of popping.",
        "      TessellationEnabled: true",
        "      TargetTriangleSize: 6",
        "      MorphRegion: 0.3",
        f"      SplatmapPath0: {ASSET_DIR}/{name}_Splat0.png",
        f"      SplatmapPath1: {ASSET_DIR}/{name}_Splat1.png",
        "      # Island-scale colour and wetness (128 = neutral, alpha 255 = dry): this island's",
        "      # rock and sand colour, its tide band, lichen, bedding and damp gullies.",
        f"      MacroMapPath: {ASSET_DIR}/{name}_Macro.png",
        "      LayerResolution: 1024",
        "      # One shared library of eight scanned layers. TilingScale is repeats across the",
        "      # tile, so it is the tile size over the metres one repeat should cover.",
        "      Layers:",
    ]
    for i, layer in enumerate(LAYERS):
        metres = layers.LIBRARY[layer][1]
        base = NEUTRAL[i]
        o += [
            f"        - Name: {layer}",
            f"          AlbedoPath: {ASSET_DIR}/Layers/{layer}_Color.jpg",
            f"          NormalPath: {ASSET_DIR}/Layers/{layer}_Normal.jpg",
            f"          ARMPath: {ASSET_DIR}/Layers/{layer}_ARM.png",
            f"          TilingScale: {size / metres:.4g}",
            "          HeightBlendSharpness: 6",
            "          TriplanarSharpness: 6",
            f"          BaseColor: [{base[0]:g}, {base[1]:g}, {base[2]:g}]",
            "          Roughness: 0.9",
            "          Metallic: 0",
        ]
    o += [
        "    # Discovery loop (issue #881): landing trigger + landmark tag. The box is the land's",
        "    # own bounding box plus 10 m, from below the waterline to above the peak, so a boat",
        "    # registers a landing as it closes the shore.",
        "    DiscoverableComponent:",
        f"      DisplayName: {name}",
        "    Rigidbody3DComponent:",
        "      BodyType: 0",
        "      IsTrigger: true",
        "    BoxCollider3DComponent:",
        f"      HalfExtents: [{half_x}, 80, {half_z}]",
        f"      Offset: [{box_x:g}, 24, {box_z:g}]",
        "    FoliageComponent:",
        "      Enabled: true",
        "      Layers:",
    ]
    for layer in FOLIAGE[name]:
        first = True
        for key, value in layer.items():
            o.append(f"        {'- ' if first else '  '}{key}: {_fmt(value)}")
            first = False
    return o


START = (0.0, -250.0)  # where the boat starts (Drift.olo, the Boat entity's Translation x, z)
BEARINGS = ("ahead", "stbd bow", "stbd abeam", "stbd quarter", "astern", "port quarter", "port abeam", "port bow")
LAYOUT_FIRST = "# LAYOUT — the island field"
LAYOUT_NEXT = "#   The sea is 1.6 km square"  # the first hand-written line after the section


def water_from_start(h, name):
    """Metres of open water from the boat's start to the island's nearest land."""
    import numpy as np
    size, _, _, corner_x, corner_z = recipes.TILES[name]
    xx, zz = _land_metres(h, size)
    return float(np.hypot(corner_x + xx - START[0], corner_z + zz - START[1]).min())


def layout_block(facts):
    """The LAYOUT section of the scene's header comment. `facts` is name -> (peak m, metres of
    open water from the start line to the island's nearest land)."""
    out = [
        LAYOUT_FIRST + " (issues #880, #1721)",
        "#   GENERATED with the island entities: tools/drift-islands/build_islands.py --stamp-scene",
        "#   writes this section, down to the paragraph about the sea, from the maps on disk.",
        "#",
        "#   The boat starts at (0, 0, -250) pointing at Ridgeback, the tallest thing on that",
        "#   bearing. Five more lie around it, at different bearings and distances, so there is",
        "#   always somewhere else to go and turning round is worth doing:",
        "#",
    ]
    for name in ("Ridgeback", "Atoll", "Stacks", "Mesa", "Dunes", "Sisters"):
        size, _, _, corner_x, corner_z = recipes.TILES[name]
        cx, cz = corner_x + size * 0.5, corner_z + size * 0.5
        peak, water = facts[name]
        # The boat heads +Z; a bearing is measured clockwise from that, +X to starboard.
        angle = math.degrees(math.atan2(cx - START[0], cz - START[1])) % 360.0
        bearing = BEARINGS[int((angle + 22.5) // 45.0) % 8]
        out.append(f"#     {name:<10s} ({cx:5.0f}, {cz:5.0f})  {size:.0f} m tile, peak {peak:3.0f} m  "
                   f"{bearing:<13s} {water:4.0f} m of water")
    out += [
        "#",
        "#   Peaks are above the sea. The last column is the open water from the start line to the",
        "#   island's nearest land.",
        "#",
        "#   They are DIFFERENT PLACES. Each is a height map the tool builds as a landform: a recipe",
        "#   draws its character in metres, erosion and a shore profile are derived from that, and",
        "#   the material maps are baked beside it. The per-island notes on each entity say what",
        "#   each one is. All six share one library of eight scanned ground layers; an island's own",
        "#   rock and sand colour comes from its macro map.",
        "#",
        "#   THE SHORELINE. A terrain tile that meets an ocean must not end in a wall: every border",
        "#   texel of every map is exactly the tile floor, and each island sits that floor 24 m",
        "#   under the sea, so a tile's edge is seabed. Inside it the coast is shaped, not left to",
        "#   a mask: sand coasts are pulled to a beach profile (a 7 degree foreshore and a berm),",
        "#   rock coasts get a wave-cut platform. docs/agent-rules/terrain-tile-meets-ocean.md has",
        "#   the rules; DriftIslandSplatCoverage pins them on the shipped maps.",
        "#",
    ]
    return out


def _anchor(lines, start, matches, what):
    """Index of the first line from `start` that matches, or an error that names what the
    stamper was looking for. An editor save drops the scene's comments, and the comments are
    the anchors: say so instead of dying on a StopIteration."""
    for i in range(start, len(lines)):
        if matches(lines[i]):
            return i
    raise ValueError(f"Drift.olo: could not find {what}. The stamper finds each block by its comments; "
                     "restore them (git checkout the scene) rather than stamping a scene saved from the editor.")


def stamp(scene_path, radii, facts, boxes):
    """Replace each island's entity block and the header's LAYOUT section. `radii` is name ->
    landfall radius in metres; `facts` is what layout_block takes; `boxes` is name -> land_box."""
    with io.open(scene_path, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    first = _anchor(lines, 0, lambda line: line.startswith(LAYOUT_FIRST), f"the header line '{LAYOUT_FIRST}'")
    last = _anchor(lines, first, lambda line: line.startswith(LAYOUT_NEXT),
                   f"the header line '{LAYOUT_NEXT}' that follows the LAYOUT section")
    lines[first:last] = layout_block(facts)
    for name, radius in radii.items():
        start = _anchor(lines, 0, lambda line: line.startswith(f"  # ── Island - {name} "),
                        f"the comment line '  # ── Island - {name} ' that opens that island's entity")
        foliage = _anchor(lines, start, lambda line: line.rstrip() == "    FoliageComponent:",
                          f"Island - {name}'s FoliageComponent")
        # The block ends at the first blank line after the foliage layers: everything from the
        # header comment to there is replaced, so nothing may be added by hand in between.
        end = _anchor(lines, foliage, lambda line: line.strip() == "", f"the blank line that ends Island - {name}'s entity")
        lines[start:end] = island_block(name, radius, boxes[name])
    with io.open(scene_path, "w", encoding="utf-8", newline="") as fh:
        fh.write(newline.join(lines))
    return os.path.basename(scene_path)

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    // One foliage type within a foliage system (grass, flowers, bushes, trees, etc.)
    struct FoliageLayer
    {
        std::string Name = "Grass";

        // Authored plant mesh (issue #1233). Rendered as REAL GEOMETRY up close
        // and baked into the octahedral impostor atlas for distance (#433).
        //
        // Authoring convention, shared with the impostor bake: base at the
        // origin, unit height (y in [0, 1]). Both paths scale the mesh
        // UNIFORMLY by the instance's `height * scale`, so a mesh authored at
        // some other size is drawn at the wrong size in BOTH — consistently,
        // never differently near vs far. FoliageRenderer reports a deviation
        // rather than silently rescaling, because rescaling here and not in the
        // bake is exactly how the near and far silhouettes drift apart.
        std::string MeshPath;

        // Albedo texture for the foliage (with alpha channel for cutout)
        std::string AlbedoPath;

        // Density / placement
        f32 Density = 1.0f;        // Instances per world unit squared
        i32 SplatmapChannel = -1;  // Splatmap channel to read density from (-1 = uniform)
        f32 MinSlopeAngle = 0.0f;  // Minimum slope angle (degrees) — 0 = flat
        f32 MaxSlopeAngle = 45.0f; // Maximum slope angle (degrees) — reject if steeper

        // Randomization
        f32 MinScale = 0.8f;
        f32 MaxScale = 1.2f;
        f32 MinHeight = 0.5f;       // Min instance height
        f32 MaxHeight = 1.5f;       // Max instance height
        bool RandomRotation = true; // Random Y-axis rotation

        // ── Species habitat rules (issue #1254) ──────────────────────────────
        //
        // What makes a scatter read as an ECOSYSTEM rather than uniform random
        // cards: each species answers "would I grow here?" from the terrain,
        // and neighbouring species answer differently, so the boundary between
        // them is where one suitability falls off as another rises.
        //
        // Every gate below produces a SUITABILITY in [0, 1] rather than a
        // yes/no, and the gates multiply. The placement test is then one
        // stochastic comparison against that product — the same test the
        // splatmap channel has always used, which is why a feathered edge
        // dissolves into its neighbour instead of ending on a line.
        //
        // EVERY ONE OF THESE DEFAULTS TO OFF. A layer authored before #1254
        // deserializes with no altitude band, no moisture band, a zero slope
        // feather and no exclusion channel, so its suitability is exactly what
        // it was: 1, or the splatmap weight. Same pattern, same reason, as
        // TransmissionStrength = 0 below.

        // Softens the existing MinSlopeAngle / MaxSlopeAngle gate by this many
        // degrees INWARD from each bound. 0 keeps the hard cut-off, evaluated
        // in exactly the cosine comparison it always was — a feathered slope
        // gate must not be able to move a plant that a zero feather kept.
        f32 SlopeFeather = 0.0f;

        // Altitude band, in WORLD units (the sampled height times the terrain's
        // height scale), not normalized heightfield units — an author reading
        // "tree line at 55 m" should be able to type 55.
        bool UseAltitudeBand = false;
        f32 MinAltitude = 0.0f;
        f32 MaxAltitude = 1000.0f;
        f32 AltitudeFeather = 0.0f; // world units of soft edge on both bounds

        // Moisture band. Moisture is a TOPOGRAPHIC PROXY derived from the
        // heightfield — low, flat ground is wet; high, steep ground is dry —
        // not a hydrology simulation, which the issue's scope boundary
        // excludes. FoliagePlacement::MoistureAt is the definition.
        bool UseMoisture = false;
        f32 MinMoisture = 0.0f;
        f32 MaxMoisture = 1.0f;
        f32 MoistureFeather = 0.0f;

        // An authored splatmap channel that SUPPRESSES this species where it is
        // painted — bare rock for grass, a path, the footprint of a building.
        // The positive mask stays SplatmapChannel above; this is the negative
        // one, and the two are independent so a layer can want grass AND not
        // want rock. -1 disables it.
        i32 ExclusionSplatmapChannel = -1;
        f32 ExclusionThreshold = 0.5f; // channel weight at which suppression is total

        // ── Clumping (issue #1254) ──────────────────────────────────────────
        //
        // Plants grow in patches. A clump field — smooth two-octave value noise
        // over the world, sampled at the plant's own position — multiplies into
        // the suitability above, so a species thins out between its patches
        // instead of covering its habitat evenly.
        //
        // STRENGTH 0 IS THE OFF SWITCH AND THE DEFAULT: the weight is a
        // constant 1 and the field is never evaluated.
        //
        // Clumping REMOVES plants (it can only multiply suitability down), so a
        // layer that gains clumping gets sparser at the same Density. Raise
        // Density to keep the same plant count inside the patches.
        f32 ClumpStrength = 0.0f;       // 0 = uniform, 1 = fully patch-driven
        f32 ClumpScale = 12.0f;         // world units — roughly one patch across
        f32 ClumpFalloff = 1.0f;        // >1 tightens patches, <1 diffuses their edges
        f32 ClumpScaleInfluence = 0.0f; // how much a patch's core grows bigger plants

        // Which clump field this species reads. -1 means its own, derived from
        // the layer's generator seed. A shared non-negative group makes two
        // species clump TOGETHER — wildflowers in the same patches as the
        // meadow grass, litter under the same trees — which is what lets a
        // habitat boundary move as one thing rather than as N independent
        // noises that average back out to uniform.
        i32 ClumpGroup = -1;

        // ── Ground contact (issue #1254) ────────────────────────────────────
        //
        // A plant is placed at the heightfield's surface, which puts the base of
        // its bounding cylinder exactly on the ground at ONE point: its origin.
        // On a slope the downhill half of a wide plant is then in the air —
        // "floating roots". Sinking it by the drop across its own half-width
        // buries the uphill side instead, which is invisible, so that is the
        // trade the sink factor makes.
        //
        // Both default to 0: a layer authored before #1254 sits exactly where
        // it sat.
        f32 GroundOffset = 0.0f;    // constant world-unit offset, + is up
        f32 SlopeSinkFactor = 0.0f; // 0 = none, 1 = the full half-width drop

        // ── Variation quality (issue #1254) ─────────────────────────────────
        //
        // The original HashPosition reinterprets a scaled float's BITS and
        // finishes with one multiply-and-shift, with the per-draw seed XORed in
        // BEFORE that multiply. Seeds that differ by a small constant therefore
        // produce outputs that differ by a near-constant offset, and the two
        // jitter draws (seed, seed + 7) are exactly that case: measured over a
        // 80x80 grid, (jitterZ - jitterX) takes just 32 distinct values, so
        // every plant in the layer sits on one of 32 diagonals inside its cell.
        // That is a visible repeated distribution at landscape scale, which
        // acceptance criterion 2 rules out.
        //
        // Setting this routes the jitter, scale, height and rotation draws
        // through HashCell — an integer avalanche hash of (cell x, cell z,
        // seed) — which gives 6398 distinct offsets over the same grid and no
        // collisions at all. Left OFF by default because turning it on MOVES
        // every plant in the layer: it changes the cell -> XZ mapping, so the
        // registry's placement signature covers it and every id in the layer
        // legitimately retires when it flips (see FoliageInstanceRegistry).
        bool DecorrelatedVariation = false;

        // LOD distances
        f32 ViewDistance = 100.0f;     // Max view distance for this layer
        f32 FadeStartDistance = 80.0f; // Distance where fade-out begins

        // Authored-mesh near field (issue #1233). With a loadable MeshPath the
        // layer draws real geometry inside MeshViewDistance and the flat card
        // (or the impostor) outside it, cross-fading across
        // [MeshFadeStartDistance, MeshViewDistance] so nothing pops. The card
        // draw carries the SAME band as its near-fade-in, so exactly one of the
        // two is opaque at any distance and both are skipped where neither is.
        bool UseAuthoredMesh = true;       // Draw MeshPath's geometry up close
        f32 MeshViewDistance = 30.0f;      // Distance where the mesh hands over to the card
        f32 MeshFadeStartDistance = 22.0f; // Distance where that hand-over begins

        // Wind
        f32 WindStrength = 0.3f; // Wind sway amplitude
        f32 WindSpeed = 1.0f;    // Wind animation speed

        f32 WindStiffness = 0.0f;
        f32 WindBranchWeight = 0.0f;
        f32 WindLeafWeight = 0.0f;
        bool WindDebugDisplacement = false;

        // ── Local interaction bending (issue #1238) ─────────────────────────
        //
        // How strongly this species answers the scene's influence field — an
        // actor walking through it. 0 means this layer ignores actors entirely
        // (a thicket of saplings should barely move where a meadow flattens),
        // and the field's own influences carry the rest of the description.
        //
        // UNLIKE the other per-layer switches above, the default is 1 rather
        // than 0, and that is deliberate: the OFF SWITCH for this feature is
        // the ABSENCE OF AN INFLUENCE SOURCE, which is exactly what every scene
        // authored before #1238 has. Such a scene therefore renders
        // bit-identically without anyone editing its layers, and a scene that
        // gains a FoliageInteractionComponent gets bending everywhere it should
        // rather than only where someone remembered to raise a slider.
        //
        // It is a RESPONSE, not a distance: the push it scales is already
        // clamped to kFoliageInteractionMaxStrength, and
        // FoliageInteractionMaximumDisplacement(this) is the world-unit bound
        // every instance AABB is padded by.
        f32 InteractionResponse = 1.0f;

        // Rendering
        glm::vec3 BaseColor{ 0.3f, 0.5f, 0.1f }; // Tint color
        f32 Roughness = 0.8f;
        f32 AlphaCutoff = 0.5f; // Alpha test threshold

        // ── Leaf material (issue #1234) ──────────────────────────────────────
        //
        // Before this, the deferred path HARD-CODED a foliage pixel's surface
        // (roughness 0.9, AO 1, the interpolated geometric normal) and the
        // forward path lit it with one directional light, no shadow and a flat
        // 0.3 ambient. `Roughness` above was authorable and read by nobody.
        // These fields are what replaces that, and they are read by the forward,
        // forward+ and deferred paths through ONE shared evaluation
        // (assets/shaders/include/FoliageSurface.glsl).
        //
        // Authored as PATHS rather than AssetHandles, matching AlbedoPath and
        // MeshPath beside them — FoliageRenderer loads them directly, as it
        // already does for the albedo cutout.
        std::string NormalMapPath;    // tangent-space leaf normals; veins and curl
        std::string RoughnessMapPath; // greyscale, MULTIPLIES Roughness above
        std::string ThicknessMapPath; // greyscale, MULTIPLIES Thickness below

        // Tangential scale of the normal map. 0 is exactly the geometric
        // normal, 1 is exactly the map.
        f32 NormalStrength = 1.0f;

        // How much light this leaf lets through, and the colour it takes on the
        // way. Transmission is what makes a backlit canopy glow instead of
        // reading as a black cutout.
        //
        // STRENGTH 0 IS THE OFF SWITCH, AND IT IS THE DEFAULT. A layer that
        // predates #1234 deserializes to 0 and renders exactly as it did:
        // MaterialKind::Generic in the G-Buffer, no transmission lobe, no
        // thickness lane. That is the conservative default a prior on-disk
        // scene is entitled to — the new material has to be asked for.
        f32 TransmissionStrength = 0.0f;
        glm::vec3 TransmissionColor{ 0.42f, 0.62f, 0.18f }; // the chlorophyll green light picks up

        // Optical thickness of the lamina, in [0, 1], modulated per pixel by
        // ThicknessMapPath. Thin tips transmit, thick midribs do not — that
        // variation is the whole point of the map.
        f32 Thickness = 0.5f;

        // Shape of the forward-scattering lobe. See oloFoliageTransmission in
        // include/FoliageSurface.glsl for what each one does to the image.
        f32 TransmissionDistortion = 0.35f; // exit direction bent back towards N
        f32 TransmissionPower = 4.0f;       // falloff exponent
        f32 TransmissionWrap = 0.5f;        // Lambertian back-face mix
        // How much of the irradiance arriving on the FAR face is transmitted.
        // This is the indirect half of the term, and it is environment
        // irradiance sampled along -N — NOT an ambient constant, which #1234
        // names as the wrong answer.
        f32 TransmissionAmbient = 0.35f;

        // Octahedral impostor LOD (issue #433) — bakes MeshPath into a view-angle
        // atlas and swaps the flat billboard for a camera-facing impostor card
        // beyond ImpostorStartDistance, cross-fading so distant trees/bushes read
        // as 3D from any azimuth instead of a flat card. Requires a valid MeshPath.
        bool UseImpostor = false;           // Enable octahedral impostor rendering for this layer
        f32 ImpostorStartDistance = 40.0f;  // Distance where the billboard cross-fades into the impostor card
        f32 ImpostorTransitionBand = 15.0f; // Cross-fade band width (world units) above the start distance
        u32 ImpostorFramesPerAxis = 8;      // Octahedral atlas grid: N frames per axis (N*N captured views)
        u32 ImpostorAtlasResolution = 1024; // Total atlas texture resolution per axis (tile res = this / N)
        bool ImpostorHemiOctahedral = true; // Hemi-octahedron (upper hemisphere) vs full sphere layout

        // ── LOD transitions and coverage-preserving density (issue #1237) ───
        //
        // The ladder above (mesh -> card -> impostor -> culled) already had
        // controllable thresholds. What it did not have is any reason for two
        // plants in a layer to cross a threshold at DIFFERENT distances, so
        // every one of them changed shape in the same frame — a ring sweeping
        // across a meadow as you walk into it — and past the last band a hard
        // alpha cut-off ended the far field on a line.
        //
        // EVERY FIELD HERE DEFAULTS TO THE IDENTITY, and a layer authored
        // before #1237 deserializes to exactly these values, so its ladder is
        // bit-identical to what it was. Same pattern, same reason, as
        // TransmissionStrength and ClumpStrength above.
        //
        // The math is FoliageLodTransition.h (and its GLSL twin); these are
        // only the authored numbers it reads.

        // World units the per-instance transition offsets spread over. The
        // spread is CENTRED on each authored threshold, so the layer still
        // hands over at the authored distance ON AVERAGE — raising this does
        // not move the ladder, it only stops it happening all at once. 0 keeps
        // every plant on the authored number.
        f32 LodTransitionSpread = 0.0f;

        // Fraction of a threshold the band moves to hold the representation a
        // plant already has: outward while the viewer retreats, inward while
        // it approaches. 0 is off. See FoliageLodTransition.h for what this
        // does and does not guarantee — it is one frame of memory, not a
        // latch, and the decorrelation above is what bounds the damage.
        f32 LodHysteresis = 0.0f;

        // Resolve a partial fade by DITHER in the passes that have no alpha to
        // blend — the deferred G-Buffer and the shadow depth pass. Off, those
        // take the hard `alpha < 0.3` cut-off they always did. This is a
        // separate switch from the density reduction below because a layer
        // with no thinning still benefits from its far fade dissolving.
        bool LodStochasticCoverage = false;

        // Coverage-preserving density reduction. As the layer thins with
        // distance the survivors are grown by exactly the factor that keeps
        // its apparent coverage constant, so a distant meadow reads as the
        // same meadow rather than as a balding one.
        bool UseDensityLod = false;
        f32 DensityLodStartDistance = 30.0f; // full density up to here
        f32 DensityLodEndDistance = 80.0f;   // the floor is reached here
        f32 DensityLodMinFraction = 0.25f;   // the floor: never thins past this
        // Width, on the per-instance hash axis, of the ramp a plant fades out
        // over as the thinning threshold crosses it. 0 makes thinning a hard
        // per-plant switch, which pops.
        f32 DensityLodFadeFraction = 0.15f;
        // Cap on the compensating growth. At the density floor the uncapped
        // factor is 1/sqrt(MinFraction) — 2x at a quarter — and a plant drawn
        // far larger than authored stops reading as the same plant. Beyond the
        // cap the layer genuinely thins, which is the honest trade.
        f32 DensityLodMaxScale = 2.0f;

        // Runtime (not serialized)
        Ref<Texture2D> AlbedoTexture;
        // Leaf maps (issue #1234), loaded by FoliageRenderer from the paths
        // above. Excluded from operator== for the same reason AlbedoTexture is.
        Ref<Texture2D> NormalTexture;
        Ref<Texture2D> RoughnessTexture;
        Ref<Texture2D> ThicknessTexture;

        bool Enabled = true;

        // Manual operator== — excludes the runtime AlbedoTexture and the leaf
        // maps beside it (a re-load of
        // the same asset hands out a different Ref pointer, which would
        // spuriously flag layers as changed). Float / glm::vec3 fields use
        // Math::BitwiseEqual per cpp-coding-quality §2a; AlbedoPath identifies
        // the texture authoritatively for equality purposes.
        auto operator==(const FoliageLayer& other) const -> bool
        {
            return Name == other.Name && MeshPath == other.MeshPath && AlbedoPath == other.AlbedoPath && Math::BitwiseEqual(Density, other.Density) && SplatmapChannel == other.SplatmapChannel && Math::BitwiseEqual(MinSlopeAngle, other.MinSlopeAngle) && Math::BitwiseEqual(MaxSlopeAngle, other.MaxSlopeAngle) && Math::BitwiseEqual(MinScale, other.MinScale) && Math::BitwiseEqual(MaxScale, other.MaxScale) && Math::BitwiseEqual(MinHeight, other.MinHeight) && Math::BitwiseEqual(MaxHeight, other.MaxHeight) && RandomRotation == other.RandomRotation && Math::BitwiseEqual(SlopeFeather, other.SlopeFeather) && UseAltitudeBand == other.UseAltitudeBand && Math::BitwiseEqual(MinAltitude, other.MinAltitude) && Math::BitwiseEqual(MaxAltitude, other.MaxAltitude) && Math::BitwiseEqual(AltitudeFeather, other.AltitudeFeather) && UseMoisture == other.UseMoisture && Math::BitwiseEqual(MinMoisture, other.MinMoisture) && Math::BitwiseEqual(MaxMoisture, other.MaxMoisture) && Math::BitwiseEqual(MoistureFeather, other.MoistureFeather) && ExclusionSplatmapChannel == other.ExclusionSplatmapChannel && Math::BitwiseEqual(ExclusionThreshold, other.ExclusionThreshold) && Math::BitwiseEqual(ClumpStrength, other.ClumpStrength) && Math::BitwiseEqual(ClumpScale, other.ClumpScale) && Math::BitwiseEqual(ClumpFalloff, other.ClumpFalloff) && Math::BitwiseEqual(ClumpScaleInfluence, other.ClumpScaleInfluence) && ClumpGroup == other.ClumpGroup && Math::BitwiseEqual(GroundOffset, other.GroundOffset) && Math::BitwiseEqual(SlopeSinkFactor, other.SlopeSinkFactor) && DecorrelatedVariation == other.DecorrelatedVariation && Math::BitwiseEqual(ViewDistance, other.ViewDistance) && Math::BitwiseEqual(FadeStartDistance, other.FadeStartDistance) && UseAuthoredMesh == other.UseAuthoredMesh && Math::BitwiseEqual(MeshViewDistance, other.MeshViewDistance) && Math::BitwiseEqual(MeshFadeStartDistance, other.MeshFadeStartDistance) && Math::BitwiseEqual(WindStrength, other.WindStrength) && Math::BitwiseEqual(WindSpeed, other.WindSpeed) && Math::BitwiseEqual(WindStiffness, other.WindStiffness) && Math::BitwiseEqual(WindBranchWeight, other.WindBranchWeight) && Math::BitwiseEqual(WindLeafWeight, other.WindLeafWeight) && WindDebugDisplacement == other.WindDebugDisplacement && Math::BitwiseEqual(InteractionResponse, other.InteractionResponse) && Math::BitwiseEqual(BaseColor, other.BaseColor) && Math::BitwiseEqual(Roughness, other.Roughness) && Math::BitwiseEqual(AlphaCutoff, other.AlphaCutoff) && NormalMapPath == other.NormalMapPath && RoughnessMapPath == other.RoughnessMapPath && ThicknessMapPath == other.ThicknessMapPath && Math::BitwiseEqual(NormalStrength, other.NormalStrength) && Math::BitwiseEqual(TransmissionStrength, other.TransmissionStrength) && Math::BitwiseEqual(TransmissionColor, other.TransmissionColor) && Math::BitwiseEqual(Thickness, other.Thickness) && Math::BitwiseEqual(TransmissionDistortion, other.TransmissionDistortion) && Math::BitwiseEqual(TransmissionPower, other.TransmissionPower) && Math::BitwiseEqual(TransmissionWrap, other.TransmissionWrap) && Math::BitwiseEqual(TransmissionAmbient, other.TransmissionAmbient) && UseImpostor == other.UseImpostor && Math::BitwiseEqual(ImpostorStartDistance, other.ImpostorStartDistance) && Math::BitwiseEqual(ImpostorTransitionBand, other.ImpostorTransitionBand) && ImpostorFramesPerAxis == other.ImpostorFramesPerAxis && ImpostorAtlasResolution == other.ImpostorAtlasResolution && ImpostorHemiOctahedral == other.ImpostorHemiOctahedral && Math::BitwiseEqual(LodTransitionSpread, other.LodTransitionSpread) && Math::BitwiseEqual(LodHysteresis, other.LodHysteresis) && LodStochasticCoverage == other.LodStochasticCoverage && UseDensityLod == other.UseDensityLod && Math::BitwiseEqual(DensityLodStartDistance, other.DensityLodStartDistance) && Math::BitwiseEqual(DensityLodEndDistance, other.DensityLodEndDistance) && Math::BitwiseEqual(DensityLodMinFraction, other.DensityLodMinFraction) && Math::BitwiseEqual(DensityLodFadeFraction, other.DensityLodFadeFraction) && Math::BitwiseEqual(DensityLodMaxScale, other.DensityLodMaxScale) && Enabled == other.Enabled;
        }
    };

    // Per-instance data for GPU (must match shader layout)
    struct FoliageInstanceData
    {
        glm::vec4 PositionScale;  // xyz = world pos, w = uniform scale
        glm::vec4 RotationHeight; // x = Y-axis rotation (radians), y = height, z = fade, w = unused
        glm::vec4 ColorAlpha;     // rgb = tint color, a = alpha cutoff
    };
} // namespace OloEngine

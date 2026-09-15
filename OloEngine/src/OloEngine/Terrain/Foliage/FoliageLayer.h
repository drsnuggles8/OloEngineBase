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
            return Name == other.Name && MeshPath == other.MeshPath && AlbedoPath == other.AlbedoPath && Math::BitwiseEqual(Density, other.Density) && SplatmapChannel == other.SplatmapChannel && Math::BitwiseEqual(MinSlopeAngle, other.MinSlopeAngle) && Math::BitwiseEqual(MaxSlopeAngle, other.MaxSlopeAngle) && Math::BitwiseEqual(MinScale, other.MinScale) && Math::BitwiseEqual(MaxScale, other.MaxScale) && Math::BitwiseEqual(MinHeight, other.MinHeight) && Math::BitwiseEqual(MaxHeight, other.MaxHeight) && RandomRotation == other.RandomRotation && Math::BitwiseEqual(ViewDistance, other.ViewDistance) && Math::BitwiseEqual(FadeStartDistance, other.FadeStartDistance) && UseAuthoredMesh == other.UseAuthoredMesh && Math::BitwiseEqual(MeshViewDistance, other.MeshViewDistance) && Math::BitwiseEqual(MeshFadeStartDistance, other.MeshFadeStartDistance) && Math::BitwiseEqual(WindStrength, other.WindStrength) && Math::BitwiseEqual(WindSpeed, other.WindSpeed) && Math::BitwiseEqual(BaseColor, other.BaseColor) && Math::BitwiseEqual(Roughness, other.Roughness) && Math::BitwiseEqual(AlphaCutoff, other.AlphaCutoff) && NormalMapPath == other.NormalMapPath && RoughnessMapPath == other.RoughnessMapPath && ThicknessMapPath == other.ThicknessMapPath && Math::BitwiseEqual(NormalStrength, other.NormalStrength) && Math::BitwiseEqual(TransmissionStrength, other.TransmissionStrength) && Math::BitwiseEqual(TransmissionColor, other.TransmissionColor) && Math::BitwiseEqual(Thickness, other.Thickness) && Math::BitwiseEqual(TransmissionDistortion, other.TransmissionDistortion) && Math::BitwiseEqual(TransmissionPower, other.TransmissionPower) && Math::BitwiseEqual(TransmissionWrap, other.TransmissionWrap) && Math::BitwiseEqual(TransmissionAmbient, other.TransmissionAmbient) && UseImpostor == other.UseImpostor && Math::BitwiseEqual(ImpostorStartDistance, other.ImpostorStartDistance) && Math::BitwiseEqual(ImpostorTransitionBand, other.ImpostorTransitionBand) && ImpostorFramesPerAxis == other.ImpostorFramesPerAxis && ImpostorAtlasResolution == other.ImpostorAtlasResolution && ImpostorHemiOctahedral == other.ImpostorHemiOctahedral && Enabled == other.Enabled;
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

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Water/WaterWake.h"
// The water-disturbance encoding contract (issue #967). Included for
// WaterDisturbance::kMaxSplatsPerFrame, which sizes WaterDisturbanceUBO's splat
// array below. Taking the constant rather than repeating the literal is the
// point: the C++ struct, the GLSL block and the CPU-side queue are then one
// number, and the header itself pulls in nothing but Core/Base.h + glm.
#include "OloEngine/Renderer/Water/WaterDisturbanceField.h"
#include <glm/glm.hpp>
#include <string>
#include <string_view>

namespace OloEngine
{
    // =============================================================================
    // UNIFORM BUFFER OBJECT STRUCTURES
    // =============================================================================

    // @brief Namespace containing all UBO structure definitions for shader binding
    //
    // These structures define the exact memory layout expected by shaders and must
    // match the corresponding GLSL uniform block layouts for proper data binding.
    namespace UBOStructures
    {
        struct CameraUBO
        {
            glm::mat4 ViewProjection;
            glm::mat4 View;
            glm::mat4 Projection;
            glm::vec3 Position;
            f32 Pad0;
            // Previous-frame view-projection for forward-path velocity
            // reconstruction in PBR_MultiLight / PBR_MultiLight_Skinned.
            // Equals ViewProjection on the first frame so velocity is zero
            // on static pixels. Shaders that do not need it declare a
            // CameraMatrices block that stops at `_padding0` (the GLSL
            // spelling of the `Pad0` above); std140 allows the C++-side
            // buffer to carry extra trailing bytes.
            glm::mat4 PrevViewProjection;
            // Camera-relative render origin (issue #429). Geometry is uploaded
            // with world positions shifted by this, so the worldPos a shader
            // sees is *relative*. Lighting/fog differences are invariant, but a
            // shader that samples an ABSOLUTE-world *pattern* (triplanar tiling,
            // procedural noise, world-anchored wave phase) must add this back:
            // absWorldPos = relativeWorldPos + u_RenderOrigin. Appended after
            // PrevViewProjection so existing shaders that stop earlier are
            // unaffected (std140 trailing-byte tolerance). Zero within the first
            // grid cell, so the add-back is a no-op near origin.
            glm::vec3 RenderOrigin = glm::vec3(0.0f);
            f32 Pad1 = 0.0f;
            // The SHADER-RECONSTRUCTION flavour of Projection (#691).
            // `Projection` above carries the rasterizer flavour (full F: y flip
            // + z remap on Vulkan), which every `gl_Position` consumer needs —
            // but a shader doing its own `(clip.z/clip.w)*0.5+0.5`, extracting
            // near/far from rows 2/3, or inverting the matrix needs the
            // row-flip-only flavour, or the z remap is double-applied
            // (ADR 0011 (59): a seam is defined by how a value is READ).
            // On GL both members are identical. Appended so every existing
            // truncated CameraMatrices declaration stays valid (std140
            // trailing-byte tolerance, same as RenderOrigin above).
            // Writers using the CAPTURE flavour for `Projection` (sky/IBL/DDGI
            // face bakes: z remap WITHOUT y flip) fill this with the RAW
            // matrix — their math sibling has neither flip nor remap.
            glm::mat4 ProjectionForReconstruction = glm::mat4(1.0f);

            static constexpr u32 GetSize()
            {
                return sizeof(CameraUBO);
            }
        };

        // std140 layout sanity check for CameraUBO — mirrors the GLSL
        // CameraMatrices block. A drift (ABI change, padding tweak, extra
        // field) fails compile-time instead of producing silently-wrong
        // matrices at runtime. Expected: 3*mat4(192) + vec3+pad(16) +
        // mat4(64) + vec3+pad(16) = 288 B (the trailing vec3 is the
        // camera-relative render origin, issue #429; the trailing mat4 is the
        // reconstruction-flavour projection, issue #691 — 288 + 64 =
        // 352). Alignment is not asserted: GLM mat4 is not 16-byte-aligned by
        // default, but the C++-side SetData() call uploads the raw byte
        // buffer so only total size matters.
        static_assert(sizeof(CameraUBO) == 352, "CameraUBO std140 size drifted from GLSL expectation (352 B)");

        // @brief Per-light record in the multi-light UBO (binding 5). Packed
        // by Scene::ProcessScene3DSharedLogic; decoded in PBRCommon.glsl /
        // PBR_MultiLight.glsl. The w-channels carry type tags and shadow
        // indices — see the Scene-side packing for the encoding.
        struct MultiLightData
        {
            glm::vec4 Position;          // Position in world space (w = 1.0 for point/spot, 0.0 for directional)
            glm::vec4 Direction;         // Direction for directional/spot lights
            glm::vec4 Color;             // Light color and intensity (w = intensity)
            glm::vec4 AttenuationParams; // (constant, linear, quadratic, range)
            glm::vec4 SpotParams;        // (inner_cutoff, outer_cutoff, falloff, type)

            static constexpr u32 GetSize()
            {
                return sizeof(MultiLightData);
            }
        };

        struct MultiLightUBO
        {
            // MAX_LIGHTS=256 produces a UBO of ~20 KB.  Renderer3D::Init()
            // validates this at runtime against GL_MAX_UNIFORM_BLOCK_SIZE.
            static constexpr u32 MAX_LIGHTS = 256; // Maximum supported lights in the array

            i32 LightCount;                    // Number of active lights
            i32 MaxLights;                     // Maximum supported lights
            i32 ShadowCasterCount;             // Number of shadow-casting lights
            i32 DirectionalLightCount;         // Number of directional lights (always at start of array)
            MultiLightData Lights[MAX_LIGHTS]; // Array of light data

            static constexpr u32 GetSize()
            {
                return sizeof(MultiLightUBO);
            }
        };

        // @brief Animation limits and constants for bone matrix uploads
        // Must match the corresponding GLSL shader array sizes
        struct AnimationConstants
        {
            static constexpr u32 MAX_BONES = 100; // Maximum bone matrices per animated mesh

            // Compile-time validation for reasonable limits
            static_assert(MAX_BONES > 0, "MAX_BONES must be positive");
            static_assert(MAX_BONES <= 200, "MAX_BONES exceeds reasonable GPU limits");
            static_assert(MAX_BONES % 4 == 0, "MAX_BONES should be multiple of 4 for optimal GPU alignment");
        };

        struct MaterialUBO
        {
            glm::vec4 Ambient;
            glm::vec4 Diffuse;
            glm::vec4 Specular; // w = shininess
            glm::vec4 Emissive;
            i32 UseTextureMaps;
            i32 AlphaMode;   // Alpha blending mode (repurposed from padding)
            i32 DoubleSided; // Double-sided rendering flag (repurposed from padding)
            i32 Pad;         // Only 4 bytes padding needed for 16-byte alignment

            static constexpr u32 GetSize()
            {
                return sizeof(MaterialUBO);
            }
        };

        struct PBRMaterialUBO
        {
            glm::vec4 BaseColorFactor;   // Base color (albedo) with alpha
            glm::vec4 EmissiveFactor;    // Emissive color
            f32 MetallicFactor;          // Metallic factor
            f32 RoughnessFactor;         // Roughness factor
            f32 NormalScale;             // Normal map scale
            f32 OcclusionStrength;       // AO strength
            i32 UseAlbedoMap;            // Use albedo texture
            i32 UseNormalMap;            // Use normal map
            i32 UseMetallicRoughnessMap; // Use metallic-roughness texture
            i32 UseAOMap;                // Use ambient occlusion map
            i32 UseEmissiveMap;          // Use emissive map
            i32 EnableIBL = 0;           // Enable IBL
            i32 ApplyGammaCorrection;    // Whether to apply gamma correction in this pass
            f32 AlphaCutoff = 0.5f;      // Alpha cutoff for MASK mode
            i32 EnableLightProbes;       // Enable light probe indirect diffuse
            f32 IBLIntensity = 1.0f;     // Runtime IBL strength multiplier
            i32 AlphaMode = 0;           // 0=Opaque, 1=Mask, 2=Blend (matches AlphaMode enum)
            i32 Pad2 = 0;

            // PER-MATERIAL HEAP OFFSETS (issue #691, ADR 0011 amendment (32)).
            //
            // WHY THESE LIVE HERE AND NOT IN THE SHARED OFFSET TABLE. That table is
            // indexed by `TEX_*` slot and published by `HeapBinding::FlushOffsets()`
            // — right for a PASS, which binds a handful of inputs once. A MATERIAL's
            // nine textures change per draw, so routing them through it would upload
            // the whole table every draw: exactly the per-draw cost bindless exists
            // to remove. This UBO is ALREADY uploaded per material, so carrying nine
            // more integers is free, and the nine `glBindTextureUnit` calls
            // BindPBRTextures used to issue go away.
            //
            // uvec4-shaped for the same std140 reason as the shared table: a
            // `uint[9]` pads to a 16-byte stride and would read every fourth entry,
            // sampling three wrong textures out of four — silently, and with
            // entirely plausible output.
            //
            // Order is fixed and mirrored by OLO_MATERIAL_* in include/BindlessHeap.glsl:
            //   [0] = .x albedo   .y metallicRoughness .z normal    .w ao
            //   [1] = .x emissive .y environment       .z irradiance .w prefilter
            //   [2] = .x brdfLut  .y diffuse (legacy)  .z specular (legacy) .w unused
            // A slot holding kNullHeapOffset means "no map"; the shader's existing
            // Use*Map flags already gate whether it samples at all.
            glm::uvec4 HeapOffsets[3]{};

            static constexpr u32 GetSize()
            {
                return sizeof(PBRMaterialUBO);
            }
        };

        struct ModelUBO
        {
            glm::mat4 Model;
            glm::mat4 Normal; // transpose(inverse(model))
            i32 EntityID;
            i32 PadEntity[3];
            // Previous-frame world transform for per-object motion vectors in
            // the deferred G-Buffer path. Equals Model for static objects or
            // on the first frame so the resulting velocity is zero. Other
            // shaders that bind UBO_MODEL ignore this tail by declaring a
            // ModelMatrices block that stops at EntityID; std140 allows the
            // C++-side buffer to carry extra trailing bytes.
            glm::mat4 PrevModel;

            static constexpr u32 GetSize()
            {
                return sizeof(ModelUBO);
            }
        };

        // std140 layout sanity check. A mismatch here means the C++-side
        // buffer no longer mirrors the GLSL ModelMatrices block and any
        // SetData() call will produce garbage (shader reads wrong offsets,
        // resulting in black geometry / broken normals / wrong entity IDs).
        // Expected: mat4(64) + mat4(64) + int+pad(16) + mat4(64) = 208 B.
        static_assert(sizeof(ModelUBO) == 208, "ModelUBO std140 size drifted from GLSL expectation (208 B)");

        struct AnimationUBO
        {
            static constexpr u32 MAX_BONES = AnimationConstants::MAX_BONES; // Use centralized constant
            glm::mat4 BoneMatrices[MAX_BONES];

            static constexpr u32 GetSize()
            {
                return sizeof(AnimationUBO);
            }

            // Compile-time validation to ensure shader compatibility
            static_assert(MAX_BONES == AnimationConstants::MAX_BONES, "AnimationUBO::MAX_BONES must match AnimationConstants::MAX_BONES");
            static_assert(sizeof(BoneMatrices) == MAX_BONES * sizeof(glm::mat4), "BoneMatrices array size mismatch");
        };

        // @brief Shader constant generation utilities
        // These functions generate GLSL preprocessor defines to inject C++ constants into shaders
        struct ShaderConstantGenerator
        {
            static std::string GetAnimationDefines()
            {
                return std::string("#define MAX_BONES ") + std::to_string(AnimationConstants::MAX_BONES) + "\n";
            }

            static std::string GetLightingDefines()
            {
                return std::string("#define MAX_LIGHTS ") + std::to_string(MultiLightUBO::MAX_LIGHTS) + "\n";
            }

            static std::string GetAllShaderDefines()
            {
                return GetAnimationDefines() + GetLightingDefines();
            }
        };

        struct IBLParametersUBO
        {
            f32 Roughness;
            f32 ExposureAdjustment; // Was padding; repurposed to serve an actual purpose
            f32 IBLIntensity;       // Was padding; repurposed to serve an actual purpose
            f32 IBLRotation;        // Environment rotation angle (repurposed from padding)

            static constexpr u32 GetSize()
            {
                return sizeof(IBLParametersUBO);
            }
        };

        // @brief Parameters for the *advanced* (quality-configurable) IBL
        // generation shaders: IBLPrefilterImportance, BRDFIntegrationAdvanced and
        // IrradianceConvolutionAdvanced. Bound at UBO_USER_0 (binding 7) during the
        // offline IBL bake — only one IBL generator shader is ever bound at a time,
        // so it can share the slot with IBLParametersUBO used by the legacy path.
        //
        // std140: a uniform block is rounded up to a multiple of vec4 (16 B); this
        // packs the active fields into the first vec4 and pads to a second so the
        // C++ struct size and the GLSL block size agree exactly.
        struct IBLAdvancedParamsUBO
        {
            f32 Roughness;             // [0,1] perceptual roughness for the current mip
            f32 QualityMultiplier;     // scales effective sample count in-shader
            i32 SampleCount;           // Monte-Carlo sample count (already quality-scaled by C++)
            i32 UseImportanceSampling; // 0/1 — fall back to a flat hemisphere sweep when 0
            i32 SourceResolution;      // source cubemap face resolution, for mip-bias firefly suppression
            i32 Pad0 = 0;
            i32 Pad1 = 0;
            i32 Pad2 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(IBLAdvancedParamsUBO);
            }
        };

        // 5 scalars (offsets 0,4,8,12,16) + 3 pad ints → 32 B, matching the
        // std140 `IBLAdvancedParams` block in IBLPrefilterImportance.glsl,
        // IrradianceConvolutionAdvanced.glsl and BRDFIntegrationAdvanced.glsl.
        static_assert(sizeof(IBLAdvancedParamsUBO) == 32, "IBLAdvancedParamsUBO std140 size drifted from GLSL expectation (32 B)");

        // @brief Terrain rendering parameters
        struct TerrainUBO
        {
            glm::vec4 WorldSizeAndHeightScale; // xy = world size X/Z, z = height scale, w = chunk size
            glm::vec4 TerrainParams;           // x = texel size, y = inv heightmap res, z = layerCount, w = triplanarSharpness
            i32 HeightmapResolution;
            // 0 = the patch VBO carries baked chunk geometry (the pre-#714 path,
            // and still what the shadow-caster draws use); 1 = the VBO is the
            // shared unit grid and the vertex stage derives its terrain rect
            // from the GPU-built visible-node list at SSBO 59.
            i32 GpuDrivenMode = 0;
            i32 GpuPatchGridRes = 0; // vertices per patch edge (K) in GPU-driven mode
            i32 Pad2 = 0;
            glm::vec4 TessFactors;          // x = inner, y = +X edge, z = -X edge, w = +Z edge
            glm::vec4 TessFactors2;         // x = -Z edge, y = morphFactor, z = LODLevel, w = tessEnabled flag
            glm::vec4 LayerTilingScales0;   // Tiling scales for layers 0-3
            glm::vec4 LayerTilingScales1;   // Tiling scales for layers 4-7
            glm::vec4 LayerBlendSharpness0; // Height blend sharpness for layers 0-3
            glm::vec4 LayerBlendSharpness1; // Height blend sharpness for layers 4-7

            // Virtual texturing (issue #715). Every terrain shader sees these
            // through the single block declaration in
            // include/TerrainParamsBlock.glsl whether or not it has a VT
            // branch — the block is declared once, so they all agree.
            //
            // Packed rather than named one-per-field because they cross into
            // GLSL as four vec4s; the ONE owner of the packing is
            // TerrainVirtualTexture::FillShaderParams, and the GLSL twin is
            // oloVTUnpackParams() in include/TerrainVirtualTexture.glsl.
            glm::vec4 VTParams0{ 0.0f }; // x = pagesWide, y = pageTexels, z = borderTexels, w = tileTexels
            glm::vec4 VTParams1{ 0.0f }; // x = cacheTexels, y = maxMip, zw = feedback dimensions
            glm::vec4 VTParams2{ 0.0f }; // x = enabled, y = feedback frame slot, z = downscale, w = log2(downscale)
            glm::vec4 VTParams3{ 0.0f }; // x = sectorsWide, y = trilinear, zw = reserved

            // The adaptive sector table (issue #715): two vec4s per
            // sector, row-major. Rides this UBO because the SSBO namespace has
            // exactly one free binding under the 84 minimum and a 2 KB fixed
            // array does not earn it. Mirrors kVTMaxSectorCount in
            // Terrain/VirtualTexture/TerrainVirtualTextureTypes.h — the
            // static_assert lives in TerrainVirtualTexture.cpp, where both are
            // visible. Packing owner: FillShaderSectorTable; GLSL twin:
            // u_TerrainVTSectors in include/TerrainParamsBlock.glsl, decoded
            // by oloVTDecodeSector().
            //   [2i]     = (uvPosX, uvPosY, uvSize, derivativeScale)
            //   [2i + 1] = (maxMip, ready, reserved, reserved)
            static constexpr u32 kTerrainVTMaxSectors = 64;
            glm::vec4 VTSectors[2 * kTerrainVTMaxSectors]{};

            static constexpr u32 GetSize()
            {
                return sizeof(TerrainUBO);
            }
        };

        // @brief GPU terrain LOD quadtree descent params (binding 79, issue #714).
        //
        // Everything the GPU descent needs, precomputed so the shader is a
        // transcription of TerrainQuadtree::SelectNode rather than a second
        // implementation of it: the planes come from Frustum::Update() and
        // ProjScale from CalculateScreenSpaceError's
        // `viewProjection[1][1] * viewportHeight * 0.5`. GLSL twin:
        // TerrainCullParams in include/TerrainCullParams.glsl.
        struct TerrainCullUBO
        {
            // Terrain-LOCAL planes (xyz = normal, w = distance), normalized and
            // ordered Near, Far, Left, Right, Top, Bottom — Frustum::Planes order.
            glm::vec4 FrustumPlanes[6];
            glm::vec4 CameraAndProjScale{ 0.0f }; // xyz = terrain-local camera, w = projection scale
            glm::vec4 SizeAndTarget{ 0.0f };      // x/y = world size X/Z, z = split threshold, w = unused
            glm::uvec4 LevelParams{ 0u };         // x = max depth, y = visible capacity, z = patch grid K, w = max seam delta
            glm::uvec4 BufferParams{ 0u };        // x = node list capacity, y = LOD map resolution, z = total nodes, w = patch index count

            static constexpr u32 GetSize()
            {
                return sizeof(TerrainCullUBO);
            }
        };

        // @brief Brush preview UBO for terrain editing visualization (binding 11)
        struct BrushPreviewUBO
        {
            glm::vec4 BrushPosAndRadius; // xyz = world position, w = radius
            glm::vec4 BrushParams;       // x = active (1.0/0.0), y = falloff, z = mode (0=sculpt, 1=paint), w = unused

            static constexpr u32 GetSize()
            {
                return sizeof(BrushPreviewUBO);
            }
        };

        struct FoliageUBO
        {
            f32 Time;
            f32 WindStrength;
            f32 WindSpeed;
            f32 ViewDistance;
            f32 FadeStart;
            f32 AlphaCutoff;
            f32 PrevTime = 0.0f; // Previous-frame time for per-fragment wind reprojection
            f32 Pad1 = 0.0f;
            glm::vec4 BaseColor; // xyz = color, w = unused

            // Octahedral impostor params (issue #433). Consumed only by the
            // Foliage_Impostor shader; zero/ignored on the flat-billboard path.
            glm::vec4 ImpostorParams0{ 0.0f }; // x=framesPerAxis, y=hemi(0/1), z=startDistance, w=transitionBand
            glm::vec4 ImpostorParams1{ 0.0f }; // x=enabled(0/1), y=meshRadius(object space), z=parallaxScale, w=unused

            static constexpr u32 GetSize()
            {
                return sizeof(FoliageUBO);
            }
        };

        // @brief Shadow mapping UBO for directional (CSM) shadows plus the
        // budgeted local-light shadow ATLAS (issue #435).
        //
        // The old fixed 4-spot / 4-point layout is replaced by a flat array of
        // atlas ENTRIES: a spot light consumes one entry, a point / sphere-area
        // light consumes six consecutive entries (cube faces in +X,-X,+Y,-Y,
        // +Z,-Z order, rendered projectively into atlas sub-rects — no more
        // linear-depth cubemaps). Each entry carries its light-space VP matrix
        // and the UV scale/offset of its tile within the atlas texture. Lights
        // reference their base entry via MultiLightData::Direction.w (UBO path)
        // or the shadow field in the Forward+ GPU light structs (cluster path);
        // -1 means no shadow.
        struct ShadowUBO
        {
            static constexpr u32 MAX_CSM_CASCADES = 4;
            // Entry budget: e.g. 12 shadowed spots + 6 shadowed point lights
            // (6 × 6 = 36 entries) fit simultaneously — far beyond the old
            // fixed 4 + 4 caps. Selection beyond the budget is priority-ranked
            // (see ShadowAtlas.h).
            static constexpr u32 MAX_SHADOW_ATLAS_ENTRIES = 48;

            glm::mat4 DirectionalLightSpaceMatrices[MAX_CSM_CASCADES]; // Light VP per cascade
            glm::vec4 CascadePlaneDistances;                           // View-space far plane per cascade
            glm::vec4 ShadowParams;                                    // x=bias, y=normalBias, z=softness, w=maxShadowDistance
            glm::mat4 AtlasEntryMatrices[MAX_SHADOW_ATLAS_ENTRIES];    // Light VP per atlas entry
            glm::vec4 AtlasEntryScaleOffset[MAX_SHADOW_ATLAS_ENTRIES]; // xy = UV scale, zw = UV offset of the entry's atlas tile
            i32 DirectionalShadowEnabled = 0;
            i32 AtlasEntryCount = 0;
            i32 ShadowMapResolution = 0; // CSM map resolution
            i32 AtlasResolution = 0;     // Atlas texture resolution
            i32 CascadeDebugEnabled = 0; // Visualize cascade boundaries
            i32 SoftShadowMode = 0;      // 0 = legacy hardware PCF, 1 = PCSS (contact-hardening)
            i32 Pad1 = 0;
            i32 Pad2 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(ShadowUBO);
            }
        };

        // std140 layout sanity check for ShadowUBO — mirrors the GLSL `ShadowData`
        // block at binding 6 (GetShadowUBOLayout). A mismatch means the C++ struct
        // drifted from the shader. Expected size:
        //   4*mat4 (256) + 2*vec4 (32) + 48*mat4 (3072) + 48*vec4 (768) + 8*int (32) = 4160 B.
        // Comfortably under the GL 4.6 16 KB minimum UBO size.
        static_assert(sizeof(ShadowUBO) % 16 == 0, "ShadowUBO must be 16-byte aligned for std140");
        static_assert(sizeof(ShadowUBO) == 4160, "ShadowUBO std140 size drifted from GLSL expectation (4160 B)");

        // @brief Decal projection parameters
        struct DecalUBO
        {
            glm::mat4 InverseDecalTransform;
            glm::mat4 InverseViewProjection; // Precomputed on CPU to avoid per-fragment inverse()
            glm::vec4 DecalColor;
            glm::vec4 DecalParams; // x = fadeDistance, y = normalAngleThreshold, z/w = unused

            static constexpr u32 GetSize()
            {
                return sizeof(DecalUBO);
            }
        };
        // @brief Light probe volume parameters for indirect diffuse GI
        struct LightProbeVolumeUBO
        {
            glm::vec4 BoundsMin;       // xyz = min corner, w = unused
            glm::vec4 BoundsMax;       // xyz = max corner, w = unused
            glm::ivec4 GridDimensions; // xyz = probe count per axis, w = total probe count
            glm::vec4 ProbeSpacing;    // xyz = spacing per axis, w = unused
            i32 Enabled;               // 1 = probes active, 0 = disabled
            f32 Intensity;             // Global intensity multiplier
            i32 Pad0 = 0;
            i32 Pad1 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(LightProbeVolumeUBO);
            }
        };

        // @brief Scene lightmap parameters (binding 1, issue #439).
        //
        // Mirrors the `LightmapData` std140 block in
        // OloEditor/assets/shaders/include/LightmapSampling.glsl. Uploaded when a
        // scene's baked lightmap is resolved (valid, non-stale) and disabled
        // otherwise — a stale bake must never be sampled, so Enabled == 0 is the
        // staleness kill switch. Per-draw atlas regions travel per-instance in
        // InstanceData::LightmapScaleOffset, not here.
        struct LightmapUBO
        {
            i32 Enabled;   // 1 = atlas bound and bake key matches the live scene
            f32 Intensity; // global baked-GI intensity multiplier
            f32 TexelSize; // 1.0 / atlas dimension (square atlas), for dilation-aware sampling
            i32 Pad0 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(LightmapUBO);
            }
        };
        // @brief Realtime DDGI probe volume parameters (binding 51, issue #632).
        //
        // Mirrors the `DDGIVolume` std140 block in
        // OloEditor/assets/shaders/include/DDGICommon.glsl. Uploaded every
        // frame while a Realtime/Hybrid LightProbeVolumeComponent is active
        // (bounds are render-origin-relative per issue #429, so a dirty-flag
        // gate would go stale when the origin rebases). Atlas tile sizes
        // (irradiance 6+2, visibility 14+2) are compile-time constants shared
        // via Renderer/DDGI/DDGICommon.h, not UBO fields.
        struct DDGIVolumeUBO
        {
            glm::vec4 BoundsMin;       // xyz = min corner (render-origin-relative), w = unused
            glm::vec4 BoundsMax;       // xyz = max corner (render-origin-relative), w = unused
            glm::ivec4 GridDimensions; // xyz = probe count per axis, w = total probe count
            glm::vec4 ProbeSpacing;    // xyz = per-axis spacing (extent/(res-1)), w = min axial spacing
            i32 Enabled;               // 1 = DDGI atlases valid and sampling active
            f32 Intensity;             // global intensity multiplier (component m_Intensity)
            f32 Hysteresis;            // temporal EMA history weight [0, 0.98]
            f32 SelfShadowBias;        // sampler bias scale (JCGT 2021 form), default 0.3
            i32 HitCacheTexels;        // per-probe hit-cache octahedral resolution (8/16/32)
            i32 FrameIndex;            // monotonically increasing DDGI update counter
            f32 HybridBlend;           // 0 = baked SH only .. 1 = DDGI only (Hybrid coverage ramp)
            f32 EnergyConservation;    // bounce-feedback albedo clamp, default 0.9
            f32 MaxRayDistance;        // cascade 0 visibility distance clamp = 1.5 * |ProbeSpacing.xyz|
            f32 BounceMarginScale;     // infinite-bounce gather margin, in probe spacings (#751)
            // --- Issue #707: cascades, sparsity, variable update rate ---
            i32 CascadeCount = 1;       // >= 1; 1 == the authored single-volume path
            f32 CascadeBlendBand = 0.f; // fraction of the half-extent; 0 == hard bounds (authored path)
            i32 UpdateRateDivisor = 1;  // relight 1-in-N probes per frame
            i32 RequestLifetime = 0;    // frames a sparsity request keeps a probe live
            i32 SparsityEnabled = 0;    // 0 = every probe is live (authored path)
            i32 Pad0 = 0;

            // Per-cascade lattice description; index 0 is the FINEST cascade.
            // Fixed-size because the block is a UBO — mirrors
            // DDGI::kMaxCascades and DDGI_MAX_CASCADES in DDGICommon.glsl, and
            // all three must move together.
            static constexpr u32 MaxCascades = 8;
            glm::vec4 CascadeOrigin[MaxCascades]{};   // xyz = render-relative world pos of lattice (0,0,0), w = max ray distance
            glm::vec4 CascadeSpacing[MaxCascades]{};  // xyz = per-axis spacing, w = min axial spacing
            glm::ivec4 CascadeLattice[MaxCascades]{}; // xyz = lattice coord stored at the window's low corner

            static constexpr u32 GetSize()
            {
                return sizeof(DDGIVolumeUBO);
            }
        };
        static_assert(sizeof(DDGIVolumeUBO) % 16 == 0, "DDGIVolumeUBO must be 16-byte aligned for std140");
        static_assert(sizeof(DDGIVolumeUBO) == 512, "DDGIVolumeUBO std140 size drifted from GLSL expectation (512 B)");
        // Still a UBO, comfortably: 512 B against the 16 KB block ceiling. The
        // cascade arrays are what make #707 cost ZERO new binding slots — the
        // engine has exactly one UBO slot left (UBO_BINDING_LIMIT = 83), and
        // spending it on a second DDGI block would have been the wrong trade.
        static_assert(sizeof(DDGIVolumeUBO) <= 16384, "DDGIVolumeUBO must stay under the 16 KB UBO block limit");

        // @brief DDGI pass-local per-draw / per-dispatch data (binding 7,
        // UBO_USER_0). Mirrors the `DDGIPassData` std140 block declared ONCE in
        // OloEditor/assets/shaders/include/DDGIPassData.glsl.
        //
        // Lives here rather than inside DDGIProbeUpdatePass.cpp (where it was
        // until issue #707) for one reason: ShaderUBOSizeConsistencyTest can
        // only guard a block whose C++ twin it can name, and an unlisted block
        // is SKIPPED rather than failed. The block grew from 160 to 400 bytes
        // for the compute stages and is now read by five shaders, which is
        // exactly the shape that drifts.
        //
        // UBO_USER_0 is a PASS-LOCAL slot — the DDGI pass owns it for its own
        // draws and dispatches and the post-process chain refills it later in
        // the frame, so this consumes no new binding.
        struct DDGIPassDataUBO
        {
            glm::mat4 Model;                                    //   0 — capture: render-relative model matrix
            glm::mat4 NormalMatrix;                             //  64 — capture: transpose(inverse(model))
            glm::vec4 BaseColor;                                // 128 — capture: material base color factor
            glm::vec4 ProbePosition;                            // 144 — xyz = render-relative probe pos, w = GLOBAL probe index
            glm::mat4 InvViewProjection;                        // 160 — PREVIOUS frame's WORLD inverse view-projection
            glm::vec4 RenderOrigin;                             // 224 — xyz = render origin (world), w = camera seed radius
            glm::vec4 CameraPosRel;                             // 240 — xyz = render-relative camera position
            glm::ivec4 ComputeParams;                           // 256 — x = total probes, y/z = screen size, w = flags
            glm::ivec4 PrevLattice[DDGIVolumeUBO::MaxCascades]; // 272 — previous per-cascade lattice min

            static constexpr u32 GetSize()
            {
                return sizeof(DDGIPassDataUBO);
            }
        };
        static_assert(sizeof(DDGIPassDataUBO) % 16 == 0, "DDGIPassDataUBO must be 16-byte aligned for std140");
        static_assert(sizeof(DDGIPassDataUBO) == 400, "DDGIPassDataUBO std140 size drifted from GLSL expectation (400 B)");
        // @brief Water surface rendering parameters
        struct WaterUBO
        {
            glm::vec4 WaveParams;            // x = Time, y = WaveSpeed, z = WaveAmplitude, w = WaveFrequency
            glm::vec4 WaveDir0;              // xy = direction0, z = steepness0, w = wavelength0
            glm::vec4 WaveDir1;              // xy = direction1, z = steepness1, w = wavelength1
            glm::vec4 WaterColor;            // rgb = shallow color, a = Transparency
            glm::vec4 WaterDeepColor;        // rgb = deep color,    a = Reflectivity
            glm::vec4 VisualParams;          // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
            glm::vec4 NormalMapScroll;       // xy = scroll0 dir, zw = scroll1 dir (scrolled by time * speed)
            glm::vec4 NormalMapSpeed;        // x = speed0, y = speed1, z = PrevTime (for Gerstner reprojection), w = renderFromBelow (1=on)
            glm::vec4 LightDirection;        // xyz = directional light dir (normalized), w = unused
            glm::vec4 ScreenParams;          // x = width, y = height, z = 1/width, w = 1/height
            glm::vec4 DepthRefractionParams; // x = depthSofteningDist, y = refractionDistortion, z = refractionHeightFactor, w = unused
            glm::vec4 RefractionColor;       // rgb = underwater tint, w = unused
            glm::vec4 FoamParams;            // x = foamHeightStart, y = foamFadeDistance, z = foamTiling, w = foamBrightness
            glm::vec4 FoamParams2;           // x = foamAngleExponent, y = shorelineFoamPower, z = sssIntensity, w = vertexSpacing (#943)
            glm::vec4 SSSColor;              // rgb = subsurface scattering color, w = foamCoverage (#943)
            glm::vec4 SSRParams;             // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
            glm::vec4 TessParams;            // x = tessellationFactor (0=disabled), y = minTessDistance, z = maxTessDistance, w = frustumCullEnable (1=on, 0=off)
            glm::vec4 FFTParams;             // x = useFFT (0=Gerstner, 1=FFT ocean), y = 1/patchSize (UV scale), z = heightScale, w = horizontalScale
            // Boat / actor wake foam field (issue #967). The world-anchored,
            // toroidally stored disturbance field produced by
            // WaterDisturbanceSystem, sampled ON TOP of the procedural,
            // shoreline and Jacobian foam above — it never replaces them.
            //
            // xy = the field window's world-XZ centre (for the edge fade),
            // z  = 1 / fieldExtentMetres (the REPEAT-wrap UV scale),
            // w  = intensity. w <= 0 IS the disabled state; there is no separate
            //      enable flag, so a frame in which the compute did not run
            //      cannot leave a stale field showing.
            // Mirrors WaterDisturbance::kInvFieldExtentMetres / WindowCentreWorld.
            glm::vec4 WakeFieldParams;
            // x = wake fade start (m), y = wake fade end (m),
            // z = edge-fade start as a normalised half-extent
            //     (WaterDisturbance::kEdgeFadeStart), w = unused.
            //
            // The wake carries its OWN distance fade rather than reusing the
            // crest-foam one, and a much longer one. That fade exists because
            // procedural whitecaps compress toward the horizon into a white
            // wash (#943); a wake is a low-frequency, world-anchored, properly
            // filtered signal with no such failure mode, and fading it out at
            // 45 m would delete the trail behind the boat under any chase
            // camera — which is the whole feature.
            glm::vec4 WakeFieldParams2;

            // Boat / actor wake SHAPE (issue #968). The analytic height + hull
            // suppression the water stages evaluate through
            // include/WaterWakeCommon.glsl, and the CPU twin
            // WaterWake::Evaluate that buoyancy floats on.
            //
            // x = live hull count, y = height scale (<= 0 disables the height
            // AND the hull flatten), z/w reserved.
            //
            // Mirrors WaterWakeSystem::GetHullCount / GetRenderHeightScale.
            glm::vec4 WakeShapeParams;
            // The packed hull records. Layout is WaterWake.h's, verbatim; a
            // flat vec4 array because its std140 stride is exactly 16 bytes on
            // every implementation, with no padding rule to get wrong.
            //
            // In WaterUBO rather than a block of its own because the engine has
            // exactly ONE UBO binding left below UBO_BINDING_LIMIT and this is
            // not what to spend it on. Water is single-instance by design, so
            // the block is uploaded once per water draw regardless of size.
            glm::vec4 WakeHulls[WaterWake::kHullVec4Count];

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(WaterUBO));
            }
        };

        // @brief One capsule disturbance as the compute shader sees it
        // (issue #967). GPU-mirror struct, so bare PascalCase fields per
        // CLAUDE.md -> Conventions.
        //
        // A capsule rather than a disc: one record covers a whole frame's hull
        // sweep, so a fast boat cannot leave a dotted trail across a dropped
        // frame. A point splat (propeller burst, impact) is P0 == P1.
        struct WaterDisturbanceSplatGpu
        {
            glm::vec4 P0Radius; // xy = p0 world XZ, z = radius (m), w = strength [0,1]
            glm::vec4 P1Shape;  // xy = p1 world XZ, z = softness (falloff exponent), w = unused
        };
        static_assert(sizeof(WaterDisturbanceSplatGpu) == 32,
                      "WaterDisturbanceSplatGpu must be two tightly packed vec4 for the std140 array stride");

        // @brief Water-disturbance field update params (binding
        // UBO_WATER_DISTURBANCE, issue #967).
        //
        // GLSL twin: the `WaterDisturbanceParams` block in
        // compute/WaterDisturbance_Update.comp. The addressing these fields
        // describe is specified once in
        // Renderer/Water/WaterDisturbanceField.h — read that before changing
        // any member here.
        //
        // The splat array is fixed-size ON PURPOSE: the bounded queue is then a
        // structural property of the format rather than a policy the CPU side
        // has to remember to enforce.
        struct WaterDisturbanceUBO
        {
            glm::ivec2 LatticeMin;                                                 // 0  — lower corner of THIS frame's window
            glm::ivec2 PrevLatticeMin;                                             // 8  — lower corner of the PREVIOUS frame's window
            f32 TexelSize;                                                         // 16 — metres per texel
            f32 DecayFactor;                                                       // 20 — this frame's multiplicative decay
            i32 Resolution;                                                        // 24 — texels per axis
            i32 SplatCount;                                                        // 28 — live entries in Splats
            i32 ResetAll;                                                          // 32 — 1 = clear the whole field this dispatch
            i32 Pad0;                                                              // 36
            i32 Pad1;                                                              // 40
            i32 Pad2;                                                              // 44
            WaterDisturbanceSplatGpu Splats[WaterDisturbance::kMaxSplatsPerFrame]; // 48

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(WaterDisturbanceUBO));
            }
        };

        // @brief Froxel volumetric fog parameters UBO (binding 46, issue #435).
        //
        // Shared by the FroxelFogScatter/FroxelFogIntegrate compute passes and
        // the fog composite fragment shader. The fog volume has its own
        // exponential depth-slice mapping (near = camera near, far =
        // clamp(FogSettings::End, 20, 500)) independent of the cluster grid's.
        struct FroxelFogUBO
        {
            glm::mat4 InverseView;        // view -> render-relative world
            glm::mat4 InverseProjection;  // clip -> view
            glm::mat4 PrevViewProjection; // ABSOLUTE world -> previous frame clip (3D temporal reprojection)
            glm::vec4 Dims;               // xyz = volume dimensions, w = temporal blend alpha
            glm::vec4 DepthParams;        // x = near, y = far, z = log2(far/near), w = frame index
            glm::vec4 RenderOrigin;       // xyz = camera-relative render origin, w = enabled (0/1)

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(FroxelFogUBO));
            }
        };

        static_assert(sizeof(FroxelFogUBO) % 16 == 0, "FroxelFogUBO must be 16-byte aligned for std140");
        static_assert(sizeof(FroxelFogUBO) == 240, "FroxelFogUBO std140 size drifted from GLSL expectation (240 B)");

        // @brief Auto-exposure metering/adaptation parameters (issue #691),
        // uploaded at UBO_AUTO_EXPOSURE (58). GLSL twin: the
        // AutoExposureParams block shared verbatim by
        // compute/AutoExposureHistogram.comp and compute/AutoExposureAverage.comp
        // — these values were bare uniforms fed by ComputeShader::Set*, which
        // the Vulkan SPIR-V route cannot express (Set* is a deliberate no-op
        // there). Every member is written once per frame before the histogram
        // dispatch; members a given shader ignores are simply unread.
        struct AutoExposureUBO
        {
            glm::vec2 MeterSize; // metering grid dimensions (capped)
            f32 MinLogLum;       // histogram lower bound (log2 luminance)
            f32 InvLogLumRange;  // 1 / (maxLogLum - minLogLum)
            f32 LogLumRange;     // maxLogLum - minLogLum
            f32 Dt;              // frame delta time (seconds)
            f32 SpeedUp;         // adaptation rate when brightening
            f32 SpeedDown;       // adaptation rate when darkening
            f32 ExposureCompensation;
            f32 MinExposure;
            f32 MaxExposure;
            f32 LowPercentile;  // metered-population band, low bound
            f32 HighPercentile; // metered-population band, high bound
            f32 Pad0;
            glm::vec2 Pad1;

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(AutoExposureUBO));
            }
        };

        static_assert(sizeof(AutoExposureUBO) % 16 == 0, "AutoExposureUBO must be 16-byte aligned for std140");
        static_assert(sizeof(AutoExposureUBO) == 64, "AutoExposureUBO std140 size drifted from GLSL expectation (64 B)");

        // @brief HZB downsample-batch parameters (issue #691),
        // uploaded at UBO_HZB (59). GLSL twin: the HZBParams block in
        // compute/HZB.comp. Refilled before EVERY 4-mip dispatch batch (the
        // values change per batch) — legal on both routes: GL re-uploads the
        // bound buffer, the Vulkan backend's arena-versioned UBOs mint a new
        // per-dispatch address on each SetData (ADR 0011 §4).
        struct HZBParamsUBO
        {
            glm::vec2 DispatchThreadIdToBufferUV; // 1/dstMip0Size (or 2/src on later batches)
            glm::vec2 InputViewportMaxBound;      // (srcSize - 0.5) / srcSize clamp bound
            glm::vec2 InvSize;                    // 1 / srcMipSize
            i32 FirstLod;                         // starting destination mip level
            i32 IsFirstPass;                      // 1 = read scene depth, 0 = read HZB
            i32 ReduceOp;                         // 0 = max (farthest), 1 = min (nearest)
            i32 Pad0;
            glm::vec2 Pad1;

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(HZBParamsUBO));
            }
        };

        static_assert(sizeof(HZBParamsUBO) % 16 == 0, "HZBParamsUBO must be 16-byte aligned for std140");
        static_assert(sizeof(HZBParamsUBO) == 48, "HZBParamsUBO std140 size drifted from GLSL expectation (48 B)");

        // @brief GTAO denoise direction (issue #691), uploaded at
        // UBO_GTAO_DENOISE (60). GLSL twin: the GTAODenoiseParams block in
        // compute/GTAO_Denoise.comp. Refilled per ping-pong pass.
        struct GTAODenoiseUBO
        {
            i32 BlurHorizontal; // 1 = horizontal, 0 = vertical
            i32 Pad0;
            glm::vec2 Pad1;

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(GTAODenoiseUBO));
            }
        };

        static_assert(sizeof(GTAODenoiseUBO) % 16 == 0, "GTAODenoiseUBO must be 16-byte aligned for std140");
        static_assert(sizeof(GTAODenoiseUBO) == 16, "GTAODenoiseUBO std140 size drifted from GLSL expectation (16 B)");

        // =====================================================================
        // Compute bare-uniform migration (issue #691)
        //
        // The eight blocks below all exist for ONE reason: GLSL-for-Vulkan
        // forbids a non-opaque uniform outside a block, so every `uniform float
        // u_Foo;` in a .comp was a hard SPIR-V compile error — and
        // VulkanComputeShader::Set* is a deliberate no-op, so even a shader that
        // did compile would read zeros. Migrating the values into std140 blocks
        // is legal on BOTH routes (GL compute at 460 core takes UBO blocks), so
        // per docs/agent-rules/glsl-shaders.md §5f the GLSL declarations do NOT
        // fork per backend.
        //
        // Where two or three shaders of one system share a parameter set, they
        // share ONE block declared VERBATIM in each file (the AutoExposure
        // precedent above) — never two different lengths at one binding, which
        // is exactly why GTAODenoise got a binding of its own.
        //
        // Members a given shader does not read are simply unread; every filler
        // is a fresh value-initialised struct, so a code path that used to skip
        // a Set* call (leaving GL's per-program uniform state at its previous
        // value) now gets a deterministic zero instead of a stale one.
        // =====================================================================

        // @brief GPU particle simulation parameters, uploaded at
        // UBO_PARTICLE_SIM (61). GLSL twin: the ParticleSimParams block shared
        // verbatim by compute/Particle_Simulate.comp (15 of the members),
        // compute/Particle_Emit.comp (EmitCount + MaxParticles) and
        // compute/Particle_Compact.comp (MaxParticles). One refill per dispatch
        // — Emit/Simulate/Compact run back to back inside one frame.
        struct GPUParticleParamsUBO
        {
            glm::vec3 Gravity;         // 0  — world-space gravity vector
            f32 DeltaTime;             // 12
            f32 DragCoefficient;       // 16
            u32 MaxParticles;          // 20
            i32 EmitCount;             // 24 — Particle_Emit only
            i32 EnableGravity;         // 28
            i32 EnableDrag;            // 32
            i32 EnableWind;            // 36
            f32 WindInfluence;         // 40
            i32 EnableNoise;           // 44
            f32 NoiseStrength;         // 48
            f32 NoiseFrequency;        // 52
            i32 EnableGroundCollision; // 56
            f32 GroundY;               // 60
            f32 CollisionBounce;       // 64
            f32 CollisionFriction;     // 68
            glm::vec2 Pad0;            // 72

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(GPUParticleParamsUBO));
            }
        };

        static_assert(sizeof(GPUParticleParamsUBO) % 16 == 0,
                      "GPUParticleParamsUBO must be 16-byte aligned for std140");
        static_assert(sizeof(GPUParticleParamsUBO) == 80,
                      "GPUParticleParamsUBO std140 size drifted from GLSL expectation (80 B)");

        // @brief Wind volume generation parameters, uploaded at
        // UBO_WIND_GENERATE (62). GLSL twin: the WindGenerateParams block in
        // compute/Wind_Generate.comp. NOT the same thing as UBO_WIND (15), the
        // WindData block every *consumer* samples — this one is the producer's
        // own grid/turbulence description and is written once per generation.
        struct WindGenerateUBO
        {
            glm::vec3 GridMin;       // 0  — world-space AABB minimum corner
            f32 GridWorldSize;       // 12 — side length of the cube (m)
            glm::vec3 WindDirection; // 16 — normalized base direction
            f32 WindSpeed;           // 28 — base speed (m/s)
            i32 GridResolution;      // 32 — voxels per axis
            f32 GustStrength;        // 36
            f32 GustFrequency;       // 40
            f32 TurbulenceIntensity; // 44
            f32 TurbulenceScale;     // 48
            f32 Time;                // 52 — accumulated seconds
            glm::vec2 Pad0;          // 56

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(WindGenerateUBO));
            }
        };

        static_assert(sizeof(WindGenerateUBO) % 16 == 0, "WindGenerateUBO must be 16-byte aligned for std140");
        static_assert(sizeof(WindGenerateUBO) == 64,
                      "WindGenerateUBO std140 size drifted from GLSL expectation (64 B)");

        // @brief Snow clipmap compute parameters, uploaded at UBO_SNOW_COMPUTE
        // (66). GLSL twin: the SnowComputeParams block shared verbatim by
        // compute/Snow_Accumulate.comp and compute/Snow_Deform.comp — the two
        // agree on the clipmap trio (Center/Extent/Resolution) and each reads
        // its own tail. Refilled before each of the two dispatches.
        struct SnowComputeUBO
        {
            glm::vec2 ClipmapCenter; // 0  — world XZ centre
            f32 ClipmapExtent;       // 8  — world-space side length
            i32 Resolution;          // 12 — texels per axis
            f32 DeltaTime;           // 16 — Snow_Accumulate only, from here down
            f32 AccumulationRate;    // 20 — m of snow per second
            f32 MaxDepth;            // 24
            f32 MeltRate;            // 28
            f32 RestorationRate;     // 32 — deformation fill-back speed (m/s)
            f32 SnowDensity;         // 36 — 0 = powder, 1 = packed
            i32 StampCount;          // 40 — Snow_Deform only
            i32 Pad0;                // 44

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(SnowComputeUBO));
            }
        };

        static_assert(sizeof(SnowComputeUBO) % 16 == 0, "SnowComputeUBO must be 16-byte aligned for std140");
        static_assert(sizeof(SnowComputeUBO) == 48, "SnowComputeUBO std140 size drifted from GLSL expectation (48 B)");

        // @brief Hydraulic-erosion droplet parameters, uploaded at
        // UBO_TERRAIN_EROSION (67). GLSL twin: the TerrainErosionParams block in
        // compute/Terrain_Erosion.comp. Refilled per erosion iteration (Seed
        // advances each call).
        struct TerrainErosionUBO
        {
            u32 Resolution;          // 0  — square heightmap resolution
            u32 MaxDropletSteps;     // 4
            u32 Seed;                // 8  — RNG seed offset
            u32 DropletCount;        // 12
            f32 Inertia;             // 16
            f32 SedimentCapacity;    // 20
            f32 MinSedimentCapacity; // 24
            f32 DepositSpeed;        // 28
            f32 ErodeSpeed;          // 32
            f32 EvaporateSpeed;      // 36
            f32 Gravity;             // 40
            f32 InitialWater;        // 44
            f32 InitialSpeed;        // 48
            i32 ErosionRadius;       // 52 — brush radius in texels
            glm::vec2 Pad0;          // 56

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(TerrainErosionUBO));
            }
        };

        static_assert(sizeof(TerrainErosionUBO) % 16 == 0, "TerrainErosionUBO must be 16-byte aligned for std140");
        static_assert(sizeof(TerrainErosionUBO) == 64,
                      "TerrainErosionUBO std140 size drifted from GLSL expectation (64 B)");

        // @brief Forward+ / clustered light-culling dispatch parameters,
        // uploaded at UBO_LIGHT_CULLING (68). GLSL twin: the LightCullingParams
        // block in compute/LightCulling.comp. Distinct from ForwardPlusUBO (7),
        // which carries the *shading*-side slice scale/bias — this one is the
        // culler's own view/projection + per-frame light counts.
        struct LightCullingUBO
        {
            glm::mat4 ViewMatrix;              // 0
            glm::mat4 InverseProjectionMatrix; // 64
            u32 PointLightCount;               // 128
            u32 SpotLightCount;                // 132
            u32 SphereAreaLightCount;          // 136
            u32 MaxLightsPerCluster;           // 140
            f32 NearPlane;                     // 144 — positive distance
            f32 FarPlane;                      // 148 — positive distance
            glm::vec2 Pad0;                    // 152

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(LightCullingUBO));
            }
        };

        static_assert(sizeof(LightCullingUBO) % 16 == 0, "LightCullingUBO must be 16-byte aligned for std140");
        static_assert(sizeof(LightCullingUBO) == 160,
                      "LightCullingUBO std140 size drifted from GLSL expectation (160 B)");

        // @brief Virtualized-geometry cluster-cull parameters, uploaded at
        // UBO_VIRTUAL_CLUSTER_CULL (69). GLSL twin: the VirtualClusterCullParams
        // block in compute/VirtualClusterCull.comp. Refilled PER DISPATCH: the
        // main view loops one dispatch per instance with InstanceIndex changing,
        // and the shadow path re-fills the whole block in ortho mode. Per-
        // dispatch SetData is the documented pattern (GL re-uploads the
        // bound buffer; the Vulkan arena mints a fresh address each SetData).
        struct VirtualClusterCullUBO
        {
            glm::mat4 OcclusionViewProjection; // 0  — the VP the bound pyramid was rendered with
            glm::vec2 HZBSize;                 // 64 — HZB texels (power-of-2)
            glm::vec2 HZBUVFactor;             // 72 — viewport / HZB size
            u32 InstanceIndex;                 // 80
            f32 ViewportHeight;                // 84 — pixels
            f32 SwRasterThresholdPixels;       // 88 — 0 disables the SW path
            i32 OrthoMode;                     // 92 — 1 = shadow cascade
            f32 OrthoErrorScale;               // 96
            i32 OcclusionEnabled;              // 100
            i32 HZBMipCount;                   // 104
            f32 OcclusionDepthBias;            // 108
            i32 WriteRejected;                 // 112 — phase 1 defers instead of dropping
            i32 Phase2;                        // 116
            u32 RejectCapacity;                // 120
            u32 CommandSlotBase;               // 124
            u32 ArgsSlotBase;                  // 128
            i32 DebugDrawClusters;             // 132 — bit field, 0 = off (issue #725)
            u32 DebugDrawClusterStride;        // 136
            u32 SwCapacity;                    // 140 — SW-raster work-list record capacity (this frame's cluster count)
            // ---- Culling-camera override (issue #726) ---------------------
            // Unlike InstanceCullUBO's unconditional field, this one is a
            // flagged override because THIS shader is bound by two views: the
            // main perspective view and the ortho shadow cascades, which pass
            // their light matrices through the camera UBO. The shadow path
            // leaves the value-initialised struct alone, so CullOverride == 0
            // and it reads the camera UBO exactly as before — the same
            // "zero-init reproduces the old behaviour" contract the rest of this
            // block relies on. The main view always sets it (frozen or not), so
            // the override path is exercised every frame rather than only while
            // the observer is on.
            glm::mat4 CullViewProjection; // 144 — render-origin-relative
            glm::vec4 CullCameraPosition; // 208 — xyz relative pos, w = override enable
            // x = |projection[1][1]| (cot(fovY/2)), y = near-plane distance —
            // the two scalars ProjectErrorPixels()/NearPlane() would otherwise
            // pull out of the camera UBO's projection. Extracted CPU-side
            // because the frozen projection is not on the GPU anywhere else.
            glm::vec4 CullProjParams; // 224

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(VirtualClusterCullUBO));
            }
        };

        static_assert(sizeof(VirtualClusterCullUBO) % 16 == 0,
                      "VirtualClusterCullUBO must be 16-byte aligned for std140");
        static_assert(sizeof(VirtualClusterCullUBO) == 240,
                      "VirtualClusterCullUBO std140 size drifted from GLSL expectation (240 B)");

        // @brief Virtualized-geometry software-raster / debug-colorize
        // parameters, uploaded at UBO_VIRTUAL_RASTER (70). GLSL twin: the
        // VirtualRasterParams block shared verbatim by
        // compute/VirtualClusterRaster.comp (both the portable and the INT64
        // variant of the same file) and compute/VirtualDebugColorize.comp —
        // whose former u_Width/u_Height were RENAMED to the raster's
        // u_ViewportWidth/u_ViewportHeight so one block serves both. Refilled
        // per dispatch: the portable raster runs two passes that differ only in
        // Phase.
        struct VirtualRasterUBO
        {
            u32 ViewportWidth;  // 0 — visibility-buffer / debug-target width
            u32 ViewportHeight; // 4
            u32 Phase;          // 8 — portable raster: 0 = depth atomicMin, 1 = payload write
            f32 OverdrawScale;  // 12 — colorize: count mapping to the hot end of the ramp

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(VirtualRasterUBO));
            }
        };

        static_assert(sizeof(VirtualRasterUBO) % 16 == 0, "VirtualRasterUBO must be 16-byte aligned for std140");
        static_assert(sizeof(VirtualRasterUBO) == 16,
                      "VirtualRasterUBO std140 size drifted from GLSL expectation (16 B)");

        // @brief GPU instance-cull parameters, uploaded at UBO_INSTANCE_CULL
        // (71). GLSL twin: the InstanceCullParams block shared verbatim by
        // compute/InstanceOcclusionCull.comp and compute/InstanceFrustumCull.comp
        // — the frustum-only variant reads the first three members and ignores
        // the occlusion tail, which the always-fresh struct leaves at zero
        // (u_OcclusionEnabled == 0 is exactly the frustum-only behaviour the
        // old "just don't call Set*" path relied on).
        struct InstanceCullUBO
        {
            glm::mat4 PrevViewProjection;  // 0  — the VP matching the bound HZB
            glm::vec4 LocalBoundingSphere; // 64 — xyz = centre, w = radius (pre-expansion)
            glm::vec2 HZBSize;             // 80
            glm::vec2 HZBUVFactor;         // 88
            u32 InstanceCount;             // 96
            f32 RadiusExpansion;           // 100
            i32 OcclusionEnabled;          // 104 — 0 = frustum only
            i32 HZBMipCount;               // 108
            f32 OcclusionDepthBias;        // 112
            i32 WriteRejected;             // 116 — phase 1 appends to the reject list
            i32 Phase2;                    // 120
            // 124 — entries the cull's compacted output (and, in phase 1, the
            // reject list) may hold. Was a pad slot, which is why this and #726's
            // CullViewProjection below are complementary rather than competing:
            // #726 added a mat4 at 128 and left the pad alone, #721 spent the
            // pad. The shader BOUND-CHECKS its atomic append against this and
            // reports the refusal through the GPU readback-stats channel
            // (issue #721) instead of writing past the allocation. Set to the
            // real allocation size in production; the
            // `GPUFrustumCuller::SetDebugOutputCapacity` knob shrinks it on
            // demand, which is how acceptance criterion #2 forces a genuine
            // truncation rather than faking a flag.
            u32 OutputCapacity; // 124
            // The CULLING camera's view-projection, made relative to this
            // frame's render origin (issue #726). Read UNCONDITIONALLY by both
            // instance-cull shaders in place of the camera UBO's
            // u_ViewProjection: the camera UBO describes the observer once the
            // culling camera is frozen, and a cull that follows the observer is
            // precisely the "plausible but not the frozen set" lie the observer
            // camera exists to rule out. Equal to the camera UBO's relative VP
            // whenever nothing is frozen, so the frozen path is not a
            // separately-rotting branch — it is the only path.
            glm::mat4 CullViewProjection; // 128

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(InstanceCullUBO));
            }
        };

        static_assert(sizeof(InstanceCullUBO) % 16 == 0, "InstanceCullUBO must be 16-byte aligned for std140");
        static_assert(sizeof(InstanceCullUBO) == 192,
                      "InstanceCullUBO std140 size drifted from GLSL expectation (192 B)");

        // @brief Ocean FFT compute-chain parameters, uploaded at UBO_OCEAN_FFT
        // (73). GLSL twin: the OceanFFTParams block shared verbatim by
        // compute/Ocean_SpectrumEvolve.comp, compute/Ocean_FFTButterfly.comp
        // and compute/Ocean_Assemble.comp (the SnowComputeUBO precedent): the
        // three agree on Resolution and each reads its own slice of the tail.
        // Refilled PER DISPATCH — Stage/Vertical change on every butterfly
        // iteration (2·log2(N) dispatches per Evaluate).
        struct OceanFFTUBO
        {
            i32 Resolution; // 0  — N (power of two), read by all three passes
            f32 PatchSize;  // 4  — L, world tile size (m) — SpectrumEvolve only
            f32 Gravity;    // 8  — g — SpectrumEvolve only
            f32 Time;       // 12 — t (seconds) — SpectrumEvolve only
            i32 Stage;      // 16 — 0 .. log2(N)-1 — FFTButterfly only
            i32 Vertical;   // 20 — 0 = rows, 1 = columns — FFTButterfly only
            f32 Choppiness; // 24 — λ, horizontal-displacement scale — Assemble only
            f32 Pad0;       // 28

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(OceanFFTUBO));
            }
        };

        static_assert(sizeof(OceanFFTUBO) % 16 == 0, "OceanFFTUBO must be 16-byte aligned for std140");
        static_assert(sizeof(OceanFFTUBO) == 32, "OceanFFTUBO std140 size drifted from GLSL expectation (32 B)");

        // @brief Cloud-noise volume bake parameters, uploaded at
        // UBO_CLOUD_NOISE_GEN (74). GLSL twin: the CloudNoiseGenParams block in
        // compute/CloudNoise_Generate.comp. Refilled per dispatch — the bake
        // runs twice (base 128³, detail 32³) at startup, not per frame.
        struct CloudNoiseGenUBO
        {
            i32 Mode;    // 0  — 0 = base, 1 = detail
            i32 Size;    // 4  — texels per axis (128 base, 32 detail)
            f32 InvSize; // 8  — 1.0 / Size (computed on the CPU)
            f32 Pad0;    // 12

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(CloudNoiseGenUBO));
            }
        };

        static_assert(sizeof(CloudNoiseGenUBO) % 16 == 0, "CloudNoiseGenUBO must be 16-byte aligned for std140");
        static_assert(sizeof(CloudNoiseGenUBO) == 16,
                      "CloudNoiseGenUBO std140 size drifted from GLSL expectation (16 B)");

        // @brief Cloud shadow-map generation parameters, uploaded at
        // UBO_CLOUD_SHADOW_GEN (75). GLSL twin: the CloudShadowGenParams block
        // in compute/CloudShadow_Generate.comp — the GENERATOR's own map
        // placement; NOT folded into CloudscapeUBO (53), whose block is shared
        // by every cloud-field consumer. Refilled once per frame.
        struct CloudShadowGenUBO
        {
            glm::vec2 ShadowCenter; // 0  — world XZ of the map center (texel-snapped)
            f32 ShadowWorldSize;    // 8  — world meters covered by the full map
            i32 ShadowResolution;   // 12 — texels per axis

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(CloudShadowGenUBO));
            }
        };

        static_assert(sizeof(CloudShadowGenUBO) % 16 == 0, "CloudShadowGenUBO must be 16-byte aligned for std140");
        static_assert(sizeof(CloudShadowGenUBO) == 16,
                      "CloudShadowGenUBO std140 size drifted from GLSL expectation (16 B)");

        // @brief Precipitation→snow accumulation feed parameters, uploaded at
        // UBO_PRECIPITATION_FEED (76). GLSL twin: the PrecipitationFeedParams
        // block in compute/Precipitation_Feed.comp. Clipmap trio first (the
        // SnowComputeUBO ordering) so the vec2 sits on its natural 8-byte
        // boundary with no implicit padding. Refilled once per update.
        struct PrecipitationFeedUBO
        {
            glm::vec2 ClipmapCenter;  // 0  — world XZ centre of the snow clipmap
            f32 ClipmapExtent;        // 8  — half-extent in world units
            i32 ClipmapResolution;    // 12 — texels per axis (e.g. 2048)
            f32 AccumulationFeedRate; // 16 — depth each landed particle contributes
            f32 GroundY;              // 20 — ground plane Y
            f32 GroundThreshold;      // 24 — ground-contact tolerance
            f32 Pad0;                 // 28

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(PrecipitationFeedUBO));
            }
        };

        static_assert(sizeof(PrecipitationFeedUBO) % 16 == 0,
                      "PrecipitationFeedUBO must be 16-byte aligned for std140");
        static_assert(sizeof(PrecipitationFeedUBO) == 32,
                      "PrecipitationFeedUBO std140 size drifted from GLSL expectation (32 B)");

        // @brief Reflection-probe cluster-cull params (issue #691),
        // uploaded at UBO_REFLECTION_PROBE_CULL (77). GLSL twin: the
        // ReflectionProbeCullParams block in compute/ReflectionProbeCull.comp.
        // The SEVENTH bare-uniform compute file — invisible to the
        // sweep because its pass never ran in that session's live log; found
        // by the first --rhi=vulkan editor launch afterwards.
        struct ReflectionProbeCullUBO
        {
            glm::mat4 ViewMatrix;              // 0   — RELATIVE world -> view
            glm::mat4 InverseProjectionMatrix; // 64
            f32 NearPlane;                     // 128
            f32 FarPlane;                      // 132
            glm::vec2 Pad0;                    // 136

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(ReflectionProbeCullUBO));
            }
        };

        static_assert(sizeof(ReflectionProbeCullUBO) % 16 == 0,
                      "ReflectionProbeCullUBO must be 16-byte aligned for std140");
        static_assert(sizeof(ReflectionProbeCullUBO) == 144,
                      "ReflectionProbeCullUBO std140 size drifted from GLSL expectation (144 B)");

        // @brief GPU prefix-sum (parallel scan) dispatch params (issue #713),
        // uploaded at UBO_PREFIX_SUM (79). GLSL twin: the PrefixSumParams block
        // declared VERBATIM in compute/PrefixSum_Scan.comp and
        // compute/PrefixSum_AddBlockOffsets.comp — one block, one layout, each
        // shader reading only the members it needs (the same convention the
        // instance-cull and particle blocks use).
        //
        // Refilled PER DISPATCH: `GPUPrefixSum::ExclusiveScanInPlace` recurses
        // over levels of block totals, and every level has its own Count.
        struct PrefixSumUBO
        {
            u32 Count = 0;          // 0  — elements in THIS level's buffer
            u32 WriteBlockSums = 0; // 4  — scan pass: emit per-work-group totals
            u32 WriteTotal = 0;     // 8  — scan pass: emit the grand total (set only at
                                    //      the single-work-group bottom of the recursion,
                                    //      where the group total IS the grand total)
            u32 Pad0 = 0;           // 12

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(PrefixSumUBO));
            }
        };

        static_assert(sizeof(PrefixSumUBO) % 16 == 0,
                      "PrefixSumUBO must be 16-byte aligned for std140");
        static_assert(sizeof(PrefixSumUBO) == 16,
                      "PrefixSumUBO std140 size drifted from GLSL expectation (16 B)");

        // @brief Volumetric cloudscape raymarch parameters (issue #633),
        // uploaded at UBO_CLOUDSCAPE (53). GLSL twin: the CloudscapeData block
        // in include/CloudscapeCommon.glsl — shared by the raymarch pass, the
        // temporal resolve, and the CloudShadow_Generate compute so every
        // consumer evaluates the same field.
        struct CloudscapeUBO
        {
            glm::vec4 Layer = glm::vec4(1500.0f, 4000.0f, 1.0f / 2500.0f, 1.0f); // bottom, top, 1/(top-bottom), density scale
            glm::vec4 Field = glm::vec4(0.35f, 0.5f, 0.5f, 0.0f);                // coverage, type blend, erosion, cloud wetness
            glm::vec4 Wind = glm::vec4(0.0f);                                    // xy = accumulated wind offset (m), z = anim scale, w = time (s)
            glm::vec4 Map = glm::vec4(1.0f / 30000.0f, 64.0f, 6.0f, 0.6f);       // 1/map extent (1/m), max steps, light steps, phase g
            glm::vec4 Light = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f);                 // sun scale, ambient scale, multi-scatter, powder
            glm::vec4 Misc = glm::vec4(0.0f);                                    // temporal blend, frame index, shadow strength, enabled
            glm::vec4 SunDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);          // xyz toward active body, w = night blend
            glm::vec4 SunColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);              // rgb = colour * intensity
            glm::vec4 Ambient = glm::vec4(0.4f, 0.5f, 0.7f, 0.0f);               // rgb = sky ambient estimate

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(CloudscapeUBO));
            }
        };

        static_assert(sizeof(CloudscapeUBO) % 16 == 0, "CloudscapeUBO must be 16-byte aligned for std140");
        static_assert(sizeof(CloudscapeUBO) == 144, "CloudscapeUBO std140 size drifted from GLSL expectation (144 B)");

        // @brief The shared MEDIA-OCCLUSION block (issues #633, #723), uploaded
        // once per frame at UBO_ATMOSPHERE_SHADING and consumed by the PBR
        // surface shaders (forward, deferred, terrain), the froxel fog scatter
        // compute, the cloud raymarch, and the volumetric-shadow generator.
        // GLSL twin: the AtmosphereShadingData block in
        // include/AtmosphereShading.glsl. A zeroed upload (default
        // construction) disables every effect it gates.
        //
        // It started as "surface weather response" (#633: wetness + the
        // top-down cloud-shadow transform). #723 widened it rather than taking
        // the last free UBO binding (83) — see the pressure note at
        // SSBO_VSM_LOCAL_LIGHTS: this block was ALREADY read by both volumetric
        // consumers, and it was already carrying a dead 64-byte
        // `CloudShadowViewProj` mat4 (declared "reserved", never written, never
        // read) that the two volumetric-shadow transforms reclaimed.
        //
        // ⚠ TWO SPACES IN ONE BLOCK, ON PURPOSE. `RelWorldToVsmTex` and
        // `Params1` are RENDER-RELATIVE (issue #429) because every consumer
        // hands them the space its fragments already carry; `VsmTexToAbsWorld`
        // is ABSOLUTE because it feeds the generator, which evaluates cloud /
        // fog density fields that are defined in absolute world space. The
        // pair is therefore NOT a matrix inverse — the names say which is which.
        struct AtmosphereShadingUBO
        {
            static constexpr u32 kVsmCascades = 2; // 0 = cloud layer, 1 = fog — mirrors VSM_CASCADE_* in the GLSL

            // Volumetric shadow map (issue #723). Cascade c occupies the
            // z-slice range [c * slicesPerCascade, (c+1) * slicesPerCascade) of
            // the shared R32F volume at TEX_VOLUMETRIC_SHADOW; these transforms
            // map into the cascade's OWN unit cube, and the sampler folds the
            // cascade offset in (see VolumetricShadowCommon.glsl).
            glm::mat4 RelWorldToVsmTex[kVsmCascades] = { glm::mat4(1.0f), glm::mat4(1.0f) }; // render-relative world -> cascade [0,1]^3
            glm::mat4 VsmTexToAbsWorld[kVsmCascades] = { glm::mat4(1.0f), glm::mat4(1.0f) }; // cascade [0,1]^3 -> ABSOLUTE world (generator)

            glm::vec4 Params0 = glm::vec4(0.0f); // x = wetness [0,1], y = cloud shadow strength [0,1],
                                                 // z = cloud shadow enabled (0/1), w = unused
            glm::vec4 Params1 = glm::vec4(0.0f); // xy = shadow map center (render-relative xz), z = world size, w = 1/world size

            // Per-cascade: x = enabled (0/1), y = strength [0,1],
            // z = march step length along the light ray (metres, = cascade
            // depth / slicesPerCascade), w = unused.
            glm::vec4 VsmParams[kVsmCascades] = { glm::vec4(0.0f), glm::vec4(0.0f) };
            // x = slices per cascade, y = cascade count, z = 1 / (slices * cascades),
            // w = XY texels per cascade (the generator's own bounds check — an
            // imageSize() on a writeonly image is legal but needlessly couples
            // the kernel to the descriptor).
            glm::vec4 VsmVolume = glm::vec4(0.0f);

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(AtmosphereShadingUBO));
            }
        };

        static_assert(sizeof(AtmosphereShadingUBO) % 16 == 0, "AtmosphereShadingUBO must be 16-byte aligned for std140");
        static_assert(sizeof(AtmosphereShadingUBO) == 336, "AtmosphereShadingUBO std140 size drifted from GLSL expectation (336 B)");

        // @brief GPU fluid solver parameters UBO (Position-Based Fluids, issue #630).
        //
        // Shared by every Fluid_*.comp pass. Uploaded once per solver step and
        // re-uploaded between constraint iterations when StepFlags.x (the
        // Jacobi ping-pong parity) changes. Kill boxes ride inline so the kill
        // pass needs no extra SSBO (kFluidMaxKillBoxes entries; Counts.w holds
        // the live count).
        struct FluidUBO
        {
            static constexpr u32 kMaxKillBoxes = 8;

            glm::vec4 BoundsMinCellSize;         // xyz = domain AABB min, w = grid cell size
            glm::vec4 BoundsMaxDt;               // xyz = domain AABB max, w = step dt
            glm::vec4 GravityH;                  // xyz = gravity, w = smoothing radius h
            glm::vec4 KernelScales;              // x = poly6 scale, y = spiky-gradient scale, z = W(deltaQ*h) (s_corr denominator), w = particle mass
            glm::vec4 PbfParams;                 // x = 1/restDensity, y = CFM epsilon, z = s_corr k, w = s_corr n
            glm::vec4 ViscosityParams;           // x = XSPH c, y = vorticity epsilon, z = max speed, w = particle radius
            glm::vec4 CouplingParams;            // x = coupling stiffness, y = impulse fixed-point scale, z = max |delta p| per iteration (kFluidMaxDeltaPFraction * h), w = Jacobi under-relaxation (kFluidJacobiRelaxation)
            glm::uvec4 GridDims;                 // xyz = grid cell counts, w = total cell count
            glm::uvec4 Counts;                   // x = max particles, y = emit count, z = body proxy count, w = kill box count
            glm::uvec4 StepFlags;                // x = Jacobi read-parity (0 = A, 1 = B), y/z/w = unused
            glm::vec4 KillBoxMin[kMaxKillBoxes]; // xyz = kill AABB min (w unused)
            glm::vec4 KillBoxMax[kMaxKillBoxes]; // xyz = kill AABB max (w unused)

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(FluidUBO));
            }
        };

        static_assert(sizeof(FluidUBO) % 16 == 0, "FluidUBO must be 16-byte aligned for std140");
        static_assert(sizeof(FluidUBO) == 416, "FluidUBO std140 size drifted from GLSL expectation (416 B)");

        // @brief Screen-space fluid rendering parameters UBO (issue #630).
        //
        // Shared by the fluid splat / smooth / composite passes. Camera
        // matrices come from the standard CameraUBO (binding 0); this block
        // carries only the fluid-specific appearance + filter tuning.
        struct FluidRenderUBO
        {
            glm::vec4 TintRadius;       // xyz = surface tint, w = particle world radius
            glm::vec4 AbsorptionParams; // xyz = Beer-Lambert absorption color, w = absorption scale
            glm::vec4 FoamParams;       // x = foam speed threshold, y = foam intensity, z/w = unused
            glm::vec4 SmoothParams;     // x = blur radius (px), y = depth falloff, z = camera near, w = camera far
            glm::vec4 ScreenParams;     // xy = viewport size (px), zw = texel size
            glm::uvec4 Counts;          // x = particle count, y = entity id (as u32), z = env cubemap bound (0/1), w = unused

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(FluidRenderUBO));
            }
        };

        static_assert(sizeof(FluidRenderUBO) % 16 == 0, "FluidRenderUBO must be 16-byte aligned for std140");
        static_assert(sizeof(FluidRenderUBO) == 96, "FluidRenderUBO std140 size drifted from GLSL expectation (96 B)");

        // @brief Forward+ clustered (froxel) light culling parameters UBO.
        //
        // The grid is a fixed-count 3D cluster grid (issue #435): X/Y screen
        // tiles scaled to the viewport, Z depth slices distributed
        // exponentially between the camera near/far planes. `Enabled` stays in
        // Params.z so the long-standing `fplus_Params.z != 0u` shader gate is
        // layout-stable across the 2D-tile -> froxel upgrade.
        //
        // Depth slice mapping (mirrored by ClusteredLighting.h helpers and
        // ForwardPlusCommon.glsl): slice = floor(log2(viewZ) * sliceScale +
        // sliceBias), where sliceScale = Z / log2(far/near) and sliceBias =
        // -Z * log2(near) / log2(far/near).
        struct ForwardPlusUBO
        {
            glm::uvec4 Params;      // x = ClusterCountX, y = ClusterCountY, z = Enabled (0/1), w = ClusterCountZ
            glm::vec4 TileScale;    // xy = clusterCount / screenSize (cluster coord per pixel), zw = unused
            glm::vec4 DepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(ForwardPlusUBO));
            }
        };

        // @brief Distance-impostor reflection probes (issue #705). GLSL twin:
        // the ReflectionProbeData block in include/ReflectionProbes.glsl,
        // uploaded at UBO_REFLECTION_PROBES by ReflectionProbeArray. Cluster
        // fields deliberately mirror ForwardPlusUBO's shapes so the shader-side
        // cluster lookup is the same math with the same slice mapping; they are
        // duplicated (not shared) so probes keep working when Forward+ is off.
        struct ReflectionProbeUBO
        {
            static constexpr u32 MAX_PROBES = 32; // one bit per probe in the cluster mask

            struct Probe
            {
                glm::vec4 PositionRadius; // xyz = render-relative world position, w = influence radius
                glm::vec4 Params;         // x = blend distance, y = intensity, z = max finite distance (dMax), w = array layer
            };

            glm::uvec4 Counts;      // x = probe count, y = cluster grid valid (0/1), z = ClusterCountX, w = ClusterCountY
            glm::vec4 TileScale;    // xy = clusterCount / screenSize, z = ClusterCountZ (as float), w = unused
            glm::vec4 DepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar
            Probe Probes[MAX_PROBES];

            static constexpr u32 GetSize()
            {
                return static_cast<u32>(sizeof(ReflectionProbeUBO));
            }
        };
        // @brief Selection outline parameters for editor entity highlighting
        struct SelectionOutlineUBO
        {
            glm::vec4 OutlineColor{ 1.0f, 0.5f, 0.0f, 0.8f }; // rgb = color, a = opacity
            glm::vec4 TexelSize{ 0.0f };                      // xy = 1/width, 1/height, zw = unused
            i32 SelectedCount = 0;                            // Number of selected entities
            i32 OutlineWidth = 1;                             // Outline width in texels
            i32 Pad0 = 0;
            i32 Pad1 = 0;
            glm::ivec4 SelectedIDs[16] = {
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
                glm::ivec4{ -1 },
            }; // 64 entity IDs packed as ivec4 (4 per vec), -1 = no entity sentinel

            static constexpr u32 MaxSelectedEntities = 64;

            static constexpr u32 GetSize()
            {
                return sizeof(SelectionOutlineUBO);
            }
        };
        // @brief GTAO (Ground Truth Ambient Occlusion) parameters
        // XeGTAO-based compute AO with HZB depth pyramid
        struct GTAOUBO
        {
            glm::vec2 NDCToViewMul; // Projection unpack (2/proj[0][0], -2/proj[1][1])
            glm::vec2 NDCToViewAdd; // Projection unpack (-1/proj[0][0], 1/proj[1][1])

            glm::vec2 NDCToViewMul_x_PixelSize; // NDCToViewMul * (1/width, 1/height)
            f32 EffectRadius = 0.5f;            // World-space AO radius
            f32 EffectFalloffRange = 0.615f;    // Relative falloff distance

            f32 RadiusMultiplier = 1.457f;      // Internal radius scaling
            f32 FinalValuePower = 2.2f;         // AO contrast curve
            f32 DenoiseBlurBeta = 1.2f;         // Edge-sensitivity for denoise
            f32 SampleDistributionPower = 2.0f; // Sample distance distribution curve

            f32 ThinOccluderCompensation = 0.0f; // Compensate thin geometry halos
            f32 DepthMIPSamplingOffset = 3.3f;   // HZB mip selection offset
            i32 NoiseIndex = 0;                  // Temporal noise frame index
            f32 DepthLinearizeA = 0.0f;          // proj[2][2]: depth linearization coeff A

            glm::vec2 HZBUVFactor{ 1.0f }; // viewportSize / hzbSize
            i32 ScreenWidth = 0;
            i32 ScreenHeight = 0;

            i32 DenoiseEnabled = 1;     // Enable/disable spatial denoise
            i32 DenoisePasses = 4;      // Number of bilateral blur passes
            f32 DepthLinearizeB = 0.0f; // proj[3][2]: depth linearization coeff B
            i32 DebugView = 0;          // 0 = off, 1 = AO only

            glm::mat4 ViewMatrix{ 1.0f }; // Camera view matrix — transforms world-space normals to view-space

            // Variable Rate Compute Shading (issue #683).
            // x = consume the per-tile rate image, y = paint the rate heatmap
            // into the AO term instead of the AO, zw reserved.
            //
            // APPENDED, never inserted. GTAO_Denoise.comp declares a SHORTER
            // PREFIX of this same `GTAOParams` block — std140 allows that, and
            // it is why UBO_GTAO_DENOISE (60) exists as a separate block rather
            // than as extra members here. Inserting a member above would
            // relayout every field the denoise shader reads, silently, with no
            // build error (glsl-shaders.md §8.2).
            glm::ivec4 VRCSParams{ 0 };

            static constexpr u32 GetSize()
            {
                return sizeof(GTAOUBO);
            }
        };
        static_assert(sizeof(GTAOUBO) % 16 == 0, "GTAOUBO must be 16-byte aligned for std140");

        // @brief Variable Rate Compute Shading classification params (issue #683).
        // GLSL twin: the `ShadingRateParams` block in
        // OloEditor/assets/shaders/compute/VRCSClassify.comp.
        //
        // Bound at UBO_USER_0 (binding 7), the shared PASS-LOCAL slot, exactly
        // like DDGIPassDataUBO — and for the same reason. The engine has ONE
        // uniform-buffer binding left below the GL 4.6 minimum of 84
        // (UBO_BINDING_LIMIT is 83); #707 and #715 both recorded the decision
        // not to spend it, and a per-dispatch parameter block for a classifier
        // that uploads immediately before its own dispatch has no claim on it
        // either. Nothing else in that dispatch reads binding 7.
        //
        // It lives in this header rather than beside the classifier so
        // ShaderUBOSizeConsistencyTest can name its C++ twin — an unlisted
        // block is SKIPPED by that test rather than failed.
        struct ShadingRateUBO
        {
            glm::ivec4 ScreenAndTiles{ 0 }; //  0 — xy = viewport pixels, zw = tile grid
            // x = relative depth range a tile may span and still coarsen,
            // y = normal spread (1 - |mean normal|), z = relative luminance
            // range, w = the tolerance multiplier the tighter 4x4 test applies
            // to all three.
            glm::vec4 Thresholds{ 0.0f }; // 16
            // x = DepthLinearizeA (proj[2][2]), y = DepthLinearizeB (proj[3][2]),
            // z = a previous-frame colour texture is bound, w = 4x4 is allowed.
            glm::vec4 ClassifyControl{ 0.0f }; // 32

            static constexpr u32 GetSize()
            {
                return sizeof(ShadingRateUBO);
            }
        };
        static_assert(sizeof(ShadingRateUBO) % 16 == 0, "ShadingRateUBO must be 16-byte aligned for std140");
        static_assert(sizeof(ShadingRateUBO) == 48, "ShadingRateUBO std140 size drifted from GLSL expectation (48 B)");
        // @brief Jump Flood Algorithm parameters for selection outline rendering
        // Used by JFA init, propagation, and composite passes
        struct JumpFloodUBO
        {
            glm::vec4 TexelSize{ 0.0f };                      // xy = 1/width, 1/height
            glm::vec4 OutlineColor{ 1.0f, 0.5f, 0.0f, 0.8f }; // rgb = color, a = opacity
            f32 OutlineThicknessInner = 0.002f;               // smoothstep inner edge
            f32 OutlineThicknessOuter = 0.004f;               // smoothstep outer edge
            i32 Step = 1;                                     // current JFA step size
            i32 Pad0 = 0;

            static constexpr u32 GetSize()
            {
                return sizeof(JumpFloodUBO);
            }
        };

        // @brief L2 spherical-harmonics coefficients for one IBL probe.
        // Used by the SH-based irradiance generator (IBL diffuse path) when
        // IBLConfiguration::UseSphericalHarmonics is enabled, and reusable by
        // any single-probe SH shader that wants a 9-coefficient lookup.
        // Layout mirrors SHCoefficients::ToGPULayout: 9 vec4 (= 144 B).
        // The first element's .w stores the validity flag (1.0 = valid).
        struct SHCoefficientsUBO
        {
            glm::vec4 Coefficients[9] = {};

            static constexpr u32 GetSize()
            {
                return sizeof(SHCoefficientsUBO);
            }
        };
    } // namespace UBOStructures

    // =============================================================================
    // FORWARD+ LIGHT CULLING GPU STRUCTURES
    // =============================================================================

    // @brief GPU-packed point light for Forward+ SSBO (matches GLSL std430 layout).
    // ShadowAndAttenuation.x carries the light's base shadow-atlas entry as a float
    // (-1 = no shadow) so the clustered shading path can attenuate by the atlas
    // (issue #435 — tile-culled lights used to be shadowless).
    // ShadowAndAttenuation.y carries the component's quadratic attenuation
    // coefficient so the clustered path evaluates the SAME falloff as the
    // brute-force MultiLightUBO path (calculateAttenuation with constant=1,
    // linear=0) — the paths must agree photometrically.
    struct GPUPointLight
    {
        glm::vec4 PositionAndRadius;    // xyz = world position, w = range/radius
        glm::vec4 ColorAndIntensity;    // xyz = color, w = intensity
        glm::vec4 ShadowAndAttenuation; // x = base atlas entry (float, -1 = none), y = quadratic attenuation coefficient, z/w = reserved
    };

    // @brief GPU-packed spot light for Forward+ SSBO (matches GLSL std430 layout)
    struct GPUSpotLight
    {
        glm::vec4 PositionAndRadius; // xyz = world position, w = range/radius
        glm::vec4 DirectionAndAngle; // xyz = direction, w = cos(outerAngle)
        glm::vec4 ColorAndIntensity; // xyz = color, w = intensity
        glm::vec4 SpotParams;        // x = cos(innerAngle), y = quadratic attenuation coefficient, z = atlas entry (float, -1 = none), w = 0
    };

    // @brief GPU-packed sphere area light for Forward+ SSBO (matches GLSL std430 layout)
    // The Karis 2013 representative-point technique consumes Position + Radius for
    // specular and uses Range for the standard distance falloff.
    struct GPUSphereAreaLight
    {
        glm::vec4 PositionAndRadius; // xyz = world position, w = emitter sphere radius
        glm::vec4 ColorAndIntensity; // xyz = color, w = intensity
        glm::vec4 RangeAndPadding;   // x = range (falloff), y = base atlas entry (float, -1 = none), z/w = reserved
    };

    static_assert(sizeof(GPUPointLight) == 48, "GPUPointLight must be 48 bytes for std430");
    static_assert(sizeof(GPUSpotLight) == 64, "GPUSpotLight must be 64 bytes for std430");
    static_assert(sizeof(GPUSphereAreaLight) == 48, "GPUSphereAreaLight must be 48 bytes for std430");

    // -------------------------------------------------------------------------
    // Forward+ light-index packing
    //
    // LightCulling.comp packs a 2-bit type tag in the top bits of each tile's
    // light index; ForwardPlusCommon.glsl reads it back. Mirrored here so C++
    // code, tests, and shaders share a single source of truth.
    //
    //   bits 31..30 — type tag
    //   bits 29..0  — index into the corresponding SSBO
    //
    // The legacy single-bit spot encoding (0x80000000) maps to tag SPOT (= 2),
    // so any shader that hasn't been recompiled keeps shading correctly until
    // a sphere-area light appears in the tile.
    // -------------------------------------------------------------------------
    namespace ForwardPlusLightIndex
    {
        inline constexpr u32 TYPE_TAG_SHIFT = 30u;
        inline constexpr u32 TYPE_TAG_POINT = 0u << TYPE_TAG_SHIFT;
        inline constexpr u32 TYPE_TAG_SPHERE_AREA = 1u << TYPE_TAG_SHIFT;
        inline constexpr u32 TYPE_TAG_SPOT = 2u << TYPE_TAG_SHIFT;
        inline constexpr u32 TYPE_TAG_MASK = 0xC0000000u;
        inline constexpr u32 INDEX_MASK = 0x3FFFFFFFu;

        static_assert(TYPE_TAG_SPOT == 0x80000000u,
                      "Spot tag must keep the legacy 0x80000000 high-bit encoding");
        static_assert((TYPE_TAG_POINT | TYPE_TAG_SPHERE_AREA | TYPE_TAG_SPOT) == 0xC0000000u,
                      "Type tags must occupy only the top two bits");
        static_assert(INDEX_MASK == (1u << TYPE_TAG_SHIFT) - 1u,
                      "Index mask must cover all bits below the tag");
    } // namespace ForwardPlusLightIndex

    // Alignment/size checks for terrain UBO structs (must match GLSL std140 layout)
    static_assert(sizeof(UBOStructures::TerrainUBO) % 16 == 0, "TerrainUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::TerrainCullUBO) % 16 == 0, "TerrainCullUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::TerrainCullUBO) == 160, "TerrainCullUBO unexpected size — update include/TerrainCullParams.glsl");
    static_assert(sizeof(UBOStructures::BrushPreviewUBO) % 16 == 0, "BrushPreviewUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::FoliageUBO) % 16 == 0, "FoliageUBO size must be 16-byte aligned for std140");
    // 144 before issue #715 appended the three virtual-texture vec4s (-> 192);
    // slices 3+4 appended VTParams3 and the 64-sector x 2-vec4 adaptive table
    // (-> 2256). Bumping this is only half the edit: the GLSL block lives in
    // include/TerrainParamsBlock.glsl and is declared ONCE for all eight
    // terrain shaders (see that file for why).
    static_assert(sizeof(UBOStructures::TerrainUBO) == 2256, "TerrainUBO unexpected size — update include/TerrainParamsBlock.glsl");
    static_assert(sizeof(UBOStructures::BrushPreviewUBO) == 32, "BrushPreviewUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::FoliageUBO) == 80, "FoliageUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::DecalUBO) % 16 == 0, "DecalUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::DecalUBO) == 160, "DecalUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::LightProbeVolumeUBO) % 16 == 0, "LightProbeVolumeUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::LightProbeVolumeUBO) == 80, "LightProbeVolumeUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::LightmapUBO) % 16 == 0, "LightmapUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::LightmapUBO) == 16, "LightmapUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::WaterUBO) % 16 == 0, "WaterUBO size must be 16-byte aligned for std140");
    // 288 until issue #967 appended WakeFieldParams / WakeFieldParams2; 320 until
    // #968 appended WakeShapeParams + WakeHulls[80] for the wake SHAPE.
    // 101 vec4 = 1616 B, comfortably under the 16 KB std140 block ceiling — and
    // an ARRAY rather than a block of its own precisely because the engine has
    // exactly one UBO binding left below UBO_BINDING_LIMIT.
    static_assert(sizeof(UBOStructures::WaterUBO) ==
                      (20u + 1u + WaterWake::kHullVec4Count) * sizeof(glm::vec4),
                  "WaterUBO no longer matches its own field list -- a member was added without "
                  "updating this expression");
    static_assert(sizeof(UBOStructures::WaterUBO) == 1616, "WaterUBO unexpected size -- update GLSL layout");
    static_assert(sizeof(UBOStructures::WaterDisturbanceUBO) % 16 == 0,
                  "WaterDisturbanceUBO size must be 16-byte aligned for std140");
    // 48 B header + kMaxSplatsPerFrame (96) * 32 B per capsule splat.
    static_assert(sizeof(UBOStructures::WaterDisturbanceUBO) == 3120,
                  "WaterDisturbanceUBO unexpected size -- update WaterDisturbance_Update.comp");
    static_assert(sizeof(UBOStructures::ForwardPlusUBO) % 16 == 0, "ForwardPlusUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::ForwardPlusUBO) == 48, "ForwardPlusUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::PBRMaterialUBO) % 16 == 0, "PBRMaterialUBO size must be 16-byte aligned for std140");
    // 96 -> 144: three uvec4 of per-material heap offsets (issue #691).
    // Every .glsl declaring PBRMaterialUBO must gain the matching trailing
    // `uvec4 u_MaterialHeapOffsets[3];` — this assert is what stops the C++ and
    // GLSL layouts drifting, which std140 would otherwise punish by silently
    // shifting every field after the divergence.
    static_assert(sizeof(UBOStructures::PBRMaterialUBO) == 144, "PBRMaterialUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::SelectionOutlineUBO) % 16 == 0, "SelectionOutlineUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::SelectionOutlineUBO) == 304, "SelectionOutlineUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::GTAOUBO) % 16 == 0, "GTAOUBO size must be 16-byte aligned for std140");
    // 176 since issue #683 appended the VRCSParams ivec4 (was 160). Appended,
    // never inserted — GTAO_Denoise.comp declares a shorter PREFIX of the same
    // GTAOParams block, so an insertion would relayout every field it reads
    // with no build error.
    static_assert(sizeof(UBOStructures::GTAOUBO) == 176, "GTAOUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::JumpFloodUBO) % 16 == 0, "JumpFloodUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::JumpFloodUBO) == 48, "JumpFloodUBO unexpected size — update GLSL layout");
    static_assert(sizeof(UBOStructures::SHCoefficientsUBO) % 16 == 0, "SHCoefficientsUBO size must be 16-byte aligned for std140");
    static_assert(sizeof(UBOStructures::SHCoefficientsUBO) == 144, "SHCoefficientsUBO unexpected size — update GLSL layout (expect 9 vec4)");

    // Standardized shader binding layout for consistent resource sharing
    // across all shaders in the engine. This ensures efficient data sharing
    // and eliminates binding conflicts.
    class ShaderBindingLayout
    {
      public:
        // =============================================================================
        // UNIFORM BUFFER OBJECT (UBO) BINDINGS
        // =============================================================================

        static constexpr u32 UBO_CAMERA = 0; // Camera matrices (view, projection, etc.)
        // Binding 1 was freed when the legacy single-light LightUBO was retired
        // (all lighting flows through UBO_MULTI_LIGHTS at binding 5); it now
        // carries the scene lightmap parameters (issue #439).
        static constexpr u32 UBO_LIGHTMAP = 1;              // Baked lightmap parameters (issue #439)
        static constexpr u32 UBO_MATERIAL = 2;              // Material properties
        static constexpr u32 UBO_MODEL = 3;                 // Model/transform matrices
        static constexpr u32 UBO_ANIMATION = 4;             // Animation/bone matrices
        static constexpr u32 UBO_MULTI_LIGHTS = 5;          // Multi-light buffer for advanced lighting
        static constexpr u32 UBO_SHADOW = 6;                // Shadow mapping matrices and parameters
        static constexpr u32 UBO_USER_0 = 7;                // User-defined buffer 0 (PostProcess)
        static constexpr u32 UBO_USER_1 = 8;                // User-defined buffer 1 (MotionBlur)
        static constexpr u32 UBO_SSAO = 9;                  // SSAO parameters
        static constexpr u32 UBO_TERRAIN = 10;              // Terrain parameters (height scale, world size, etc.)
        static constexpr u32 UBO_BRUSH_PREVIEW = 11;        // Brush preview overlay for terrain editing
        static constexpr u32 UBO_FOLIAGE = 12;              // Foliage instance rendering parameters
        static constexpr u32 UBO_SNOW = 13;                 // Snow rendering parameters
        static constexpr u32 UBO_SSS = 14;                  // SSS blur parameters
        static constexpr u32 UBO_WIND = 15;                 // Wind system parameters
        static constexpr u32 UBO_SNOW_ACCUMULATION = 16;    // Snow accumulation clipmap parameters
        static constexpr u32 UBO_FOG = 17;                  // Fog & atmospheric scattering parameters
        static constexpr u32 UBO_PRECIPITATION = 18;        // Precipitation system parameters
        static constexpr u32 UBO_PRECIPITATION_SCREEN = 19; // Precipitation screen-space effects (streaks + lens)
        static constexpr u32 UBO_FOG_VOLUMES = 20;          // Local fog volume data (array of volumes)
        static constexpr u32 UBO_DECAL = 21;                // Decal projection parameters
        static constexpr u32 UBO_LIGHT_PROBES = 22;         // Light probe volume parameters
        static constexpr u32 UBO_WATER = 23;                // Water surface rendering parameters
        static constexpr u32 UBO_SHADER_GRAPH = 24;         // Shader graph user parameters
        static constexpr u32 UBO_FORWARD_PLUS = 25;         // Forward+ tile-based culling parameters
        static constexpr u32 UBO_BOOT = 26;                 // Boot/warmup shader progress data
        static constexpr u32 UBO_SELECTION_OUTLINE = 27;    // Selection outline parameters (editor)
        static constexpr u32 UBO_GTAO = 28;                 // GTAO (Ground Truth AO) parameters
        static constexpr u32 UBO_JUMP_FLOOD = 29;           // Jump Flood Algorithm parameters (editor)
        static constexpr u32 UBO_DEFERRED_LIGHTING = 30;    // Deferred lighting composition controls
        static constexpr u32 UBO_ANIMATION_PREV = 31;       // Previous-frame bone matrices (Deferred G-Buffer per-bone velocity)
        static constexpr u32 UBO_TAA = 32;                  // Temporal Anti-Aliasing parameters
        static constexpr u32 UBO_DRS = 33;                  // Dynamic Resolution Scaling bounds
        static constexpr u32 UBO_PREVIEW = 34;              // Content-browser asset thumbnail preview (matrices + material factors)
        static constexpr u32 UBO_SH_COEFFICIENTS = 35;      // L2 spherical-harmonics coefficients (9 vec4) for SH-based IBL irradiance
        static constexpr u32 UBO_PROCEDURAL_SKY = 36;       // Preetham analytic sky model coefficients (PreethamCoefficientsUBO, 8 vec4)
        static constexpr u32 UBO_UNDERWATER = 37;           // Underwater fog parameters (camera-below-water tint, WATER_FUTURE_IMPROVEMENTS.md §7.2)
        static constexpr u32 UBO_SSR = 38;                  // Screen-space reflections parameters (camera matrices + ray-march settings)
        static constexpr u32 UBO_STAR_NEST_SKY = 39;        // Star Nest raymarched nebula sky parameters (StarNestSkyUBO, 4 vec4)
        static constexpr u32 UBO_SSGI = 40;                 // Screen-space global illumination parameters (camera matrices + hemisphere ray-march settings)
        static constexpr u32 UBO_CONTACT_SHADOW = 41;       // Screen-space contact shadows parameters (camera matrices + toward-light dir + ray-march settings)
        static constexpr u32 UBO_MOTION_BLUR_PARAMS = 42;   // Motion-blur per-pass flags (hasVelocity gate: per-pixel velocity vs camera-only reconstruction)
        static constexpr u32 UBO_PLANAR_REFLECTION = 43;    // Planar-reflection mirror view-projection + plane/enable params (sampled by Water.glsl)
        static constexpr u32 UBO_UPSCALER = 44;             // Spatial upscaler / CAS·RCAS sharpening params (sharpness + texel size) — PostProcess_CAS.glsl / PostProcess_RCAS.glsl
        static constexpr u32 UBO_EASU = 45;                 // FSR1 EASU upscale constants (input/output size + tap texel + DRS bounds) — PostProcess_EASU.glsl
        static constexpr u32 UBO_FROXEL_FOG = 46;           // Froxel volumetric fog params (volume dims, depth slicing, temporal reprojection — issue #435)
        static constexpr u32 UBO_FLUID = 47;                // GPU fluid solver params (PBF kernels, grid dims, Jolt coupling — issue #630)
        static constexpr u32 UBO_FLUID_RENDER = 48;         // Screen-space fluid rendering params (tint, absorption, foam, smoothing — issue #630)
        static constexpr u32 UBO_VIRTUAL_DRAW = 49;         // Virtualized-geometry per-MDI-call draw info: instance index + command segment base (issue #629)
        static constexpr u32 UBO_VIRTUAL_DEBUG = 50;        // Virtualized-geometry debug-visualization mode (cluster/LOD/overdraw) — issue #629
        static constexpr u32 UBO_DDGI = 51;                 // Realtime DDGI probe volume params (bounds, grid, hysteresis, hybrid blend — issue #632)
        static constexpr u32 UBO_ATMOSPHERE_SKY = 52;       // Combined day/night atmosphere sky bake params (AtmosphereSkyUBO, 11 vec4 — issue #633)
        static constexpr u32 UBO_CLOUDSCAPE = 53;           // Volumetric cloudscape raymarch params (CloudscapeUBO — issue #633)
        static constexpr u32 UBO_ATMOSPHERE_SHADING = 54;   // Surface weather response: wetness + cloud-shadow map transform + enables (AtmosphereShadingUBO — issue #633)
        static constexpr u32 UBO_IMPOSTOR_BAKE = 55;        // Octahedral impostor atlas bake params (view-proj + center/radius/cutoff/tint — issue #433)
        static constexpr u32 UBO_AUTO_EXPOSURE = 72;        // (was 58 until the #705 reflection-probe
                                                            // block claimed 58 on master — moved on merge;
                                                            // GLSL twins in the two AutoExposure*.comp files
                                                            // move with it)        // Auto-exposure metering/adaptation params (AutoExposureUBO — issue #691: the histogram/average computes' former bare uniforms, which the Vulkan SPIR-V route cannot express)
        static constexpr u32 UBO_HZB = 59;                  // HZB downsample-batch params (HZBParamsUBO — issue #691: HZB.comp's former "push-constant-style" bare uniforms; refilled per 4-mip batch)
        static constexpr u32 UBO_GTAO_DENOISE = 60;         // GTAO denoise direction (GTAODenoiseUBO — issue #691: the per-ping-pong-pass blur axis; NOT folded into UBO_GTAO 28, whose GLSL block is declared at two different lengths across GTAO.comp / GTAO_Denoise.comp)
        // The compute bare-uniform sweep (issue #691). Every one of
        // these replaces a set of default-block `uniform` declarations that the
        // Vulkan SPIR-V route rejects outright and whose ComputeShader::Set*
        // feeders are a no-op there. See the UBOStructures twins above for the
        // exact member lists and which shaders share which block.
        static constexpr u32 UBO_PARTICLE_SIM = 61;  // GPU particle simulate/emit/compact params (GPUParticleParamsUBO)
        static constexpr u32 UBO_WIND_GENERATE = 62; // Wind volume GENERATION params (WindGenerateUBO) — the producer side; UBO_WIND (15) is what consumers sample
        // 63, 64 and 65 are deliberately skipped in the UNIFORM-BUFFER
        // namespace: they are SSBO_BONE_PULL (63), TEX_DDGI_VISIBILITY (64) and
        // TEX_VSM_PHYSICAL (65 since #702; this comment used to say
        // TEX_SHADER_GRAPH_0, which has since moved twice and now sits at 67).
        // The backend maps each resource kind with its own
        // VK_SPIRV_RESOURCE_TYPE_* mask (VulkanPipelineBuilder::
        // BuildBindingMappings), so a cross-namespace reuse is legal — but the
        // A2 note above earned the habit of keeping engine slots numerically
        // unique across namespaces where there is room.
        //
        // THAT ROOM IS GONE ABOVE ~62, and pretending otherwise would be worse
        // than saying so: UBOs now run to 82 and textures must stay under 80
        // (the GL 4.6 combined-unit minimum), so EVERY remaining texture number
        // collides with some UBO or SSBO. TEX_VSM_PHYSICAL (65) already shares
        // its number with SSBO_TERRAIN_NODE_BOUNDS, and #723's
        // TEX_VOLUMETRIC_SHADOW (66) shares its with UBO_SNOW_COMPUTE. The rule
        // that still holds — and the only one A2 was ever really about — is the
        // WITHIN-SHADER one: no single shader may use the same number in two
        // namespaces, because Vulkan's single-set model collapses them. Check
        // the include tree, not the table.
        // Water-disturbance field update params (WaterDisturbanceUBO, issue
        // #967) — the compute's window/decay header plus its fixed-size splat
        // array. One of the three numbers the note above reserved out of the
        // UNIFORM-BUFFER namespace; the SSBO namespace is full at 0..83, so
        // this is the only kind of binding left to claim.
        //
        // The within-shader rule is what actually has to hold, and it does:
        // WaterDisturbance_Update.comp declares this block and nothing else at
        // 63 — it has no SSBO at all, and its only other resource is storage
        // image unit 0, which lives in the rebased image namespace.
        // SSBO_BONE_PULL (63) is a Vulkan-only vertex-pull stream that no
        // compute shader touches.
        static constexpr u32 UBO_WATER_DISTURBANCE = 63;
        static constexpr u32 UBO_SNOW_COMPUTE = 66;         // Snow accumulate/deform clipmap params (SnowComputeUBO)
        static constexpr u32 UBO_TERRAIN_EROSION = 67;      // Hydraulic-erosion droplet params (TerrainErosionUBO)
        static constexpr u32 UBO_LIGHT_CULLING = 68;        // Forward+/clustered light-cull dispatch params (LightCullingUBO) — the CULLER's view/counts; UBO_FORWARD_PLUS (25) is the shading side
        static constexpr u32 UBO_VIRTUAL_CLUSTER_CULL = 69; // Virtual-geometry cluster cull params (VirtualClusterCullUBO) — refilled per instance dispatch
        static constexpr u32 UBO_VIRTUAL_RASTER = 70;       // Virtual-geometry SW raster + debug colorize params (VirtualRasterUBO)
        static constexpr u32 UBO_INSTANCE_CULL = 71;        // GPU instance frustum/occlusion cull params (InstanceCullUBO)
        // The completion of the same sweep — the six compute shaders
        // whose passes never ran in the live log but carried the
        // identical bare-uniform debt (issue #691).
        static constexpr u32 UBO_OCEAN_FFT = 73;             // Ocean FFT chain params (OceanFFTUBO) — one block shared by SpectrumEvolve/FFTButterfly/Assemble, refilled per dispatch
        static constexpr u32 UBO_CLOUD_NOISE_GEN = 74;       // Cloud-noise volume bake params (CloudNoiseGenUBO) — startup bake, two dispatches
        static constexpr u32 UBO_CLOUD_SHADOW_GEN = 75;      // Cloud shadow-map generation params (CloudShadowGenUBO) — the generator side; consumers read UBO_ATMOSPHERE_SHADING (54)
        static constexpr u32 UBO_PRECIPITATION_FEED = 76;    // Precipitation→snow feed params (PrecipitationFeedUBO) — NOT UBO_PRECIPITATION (18), the render-side block
        static constexpr u32 UBO_REFLECTION_PROBE_CULL = 77; // Reflection-probe cluster-cull params (ReflectionProbeCullUBO) — the seventh bare-uniform file, found live

        // (The GL 4.6 GL_MAX_UNIFORM_BUFFER_BINDINGS minimum guarantee of 84 is
        // asserted once, against UBO_BINDING_LIMIT, below — naming a single
        // hand-picked constant here is what let the range drift unnoticed.)
        // The heap-offset table (issue #691). std140 uvec4[] of
        // RHI::HeapOffset values, indexed by the SAME TEX_* constant a slot-based
        // shader would have used in `layout(binding = N)`. That reuse is the
        // point: it is ADR 0011 §1.1's "the number survives, promoted from a
        // compile-time constant to runtime data", and it makes the bindless and
        // slot-based variants of one shader structurally unable to disagree
        // about which texture is which. Written by
        // RGCommandContext::BindTextureOrHeapOffset, read via
        // include/BindlessHeap.glsl's OLO_HEAP_* macros.
        static constexpr u32 UBO_HEAP_OFFSETS = 56;
        // GPU-pushable shader debug draws (issue #725): the render-side params
        // for DebugDrawPrimitives.glsl — main-camera view-projection, the
        // observer-camera inverse view-projection that gives meaning to the
        // ObserverCameraNDC coordinate space, viewport size + line width (the
        // expansion is a screen-space quad, so it needs pixels), and the
        // primitive selector that picks which channel this draw expands.
        // Push-side shaders never see this block — they only touch the SSBOs.
        static constexpr u32 UBO_DEBUG_DRAW = 57;
        // Distance-impostor reflection probe set (issue #705): per-frame probe
        // array (positions, radii, blend, intensity, dMax, layer) + the
        // cluster-lookup params for the per-cluster probe mask. Uploaded by
        // ReflectionProbeArray, read by the deferred/forward lit passes and
        // the ReflectionProbeCull compute.
        static constexpr u32 UBO_REFLECTION_PROBES = 58;
        // Colour-vision deficiency adaptation params (issue #458): mode,
        // severity, correct-vs-simulate, display gamma. Read by
        // PostProcess_ColorBlind.glsl; the C++ twin is
        // OloEngine/Accessibility/AccessibilitySettings.h's ColorBlindUBOData.
        // Deliberately NOT folded into UBO_USER_0 (PostProcessUBOData): that
        // block is full, and growing it would change the std140 layout every
        // PostProcess_*.glsl declares.
        // 78, NOT 73: issue #691 claimed 73 for UBO_OCEAN_FFT while this
        // branch was open. Two UBOs sharing a binding point is silent data
        // corruption, not a build error, so this moved on merge — and the GLSL
        // block in PostProcess_ColorBlind.glsl plus ColorBlindMath's binding
        // assertion move with it.
        static constexpr u32 UBO_COLORBLIND = 78;

        // GPU prefix-sum / parallel-scan dispatch params (issue #713).
        // PrefixSumUBO — Count + the two "what does this level emit" flags,
        // refilled per dispatch by GPUPrefixSum's recursion over block totals.
        static constexpr u32 UBO_PREFIX_SUM = 79;

        // GPU terrain LOD quadtree descent params (TerrainCullUBO — issue #714):
        // the six terrain-LOCAL frustum planes, the camera position, the
        // precomputed projection scale and the tree/buffer dimensions. Shared
        // verbatim by all four Terrain*.comp kernels through
        // include/TerrainCullParams.glsl. NOT folded into UBO_TERRAIN (10) —
        // that block is the RENDER side, declared by five terrain .glsl files
        // across three stages each, and growing it would relayout every one of
        // them for data no drawing stage reads.
        //
        // 80, not 79: #714 and #713 were developed in parallel and both claimed
        // 79. #713 landed first (as #819), so it keeps the number — two UBOs on
        // one binding is silent data corruption, not a build error.
        static constexpr u32 UBO_TERRAIN_CULL = 80;

        // Virtual Shadow Maps (issue #702). GLSL twins:
        // include/VirtualShadowResources.glsl's VirtualShadowGlobals and
        // VirtualShadowPass blocks.
        //
        // The globals block is read by the nine VSM compute kernels, by the depth
        // raster AND by every lit shader that samples the map — one upload, one
        // set of clip projections, so a producer and a consumer cannot disagree
        // about which clip level covers a world position (the archetypal VSM bug:
        // mark level L, sample level L+1, read an unallocated page, render
        // unshadowed). The pass block is per-dispatch/per-draw scratch, refilled
        // immediately before each use per the #691 pattern.
        //
        // 81/82, third numbering in one PR: #713 took 79 and #714 took 80 while
        // this branch was in flight, and the LATER arrival is the one that moves
        // — a shipped binding cannot be renumbered without touching every shader
        // that already names it. One UBO slot now remains below the GL 4.6
        // minimum guarantee of 84; the static_assert below is the tripwire.
        static constexpr u32 UBO_VIRTUAL_SHADOW = 81;
        static constexpr u32 UBO_VIRTUAL_SHADOW_DRAW = 82;

        // ONE past the highest engine UBO binding above. Every consumer that
        // needs to size an array over "all UBO bindings" derives it from here
        // instead of naming a hand-picked constant — GLStateGuard's UBO-leak
        // snapshot and VulkanBindingState's bind-point mirror both did, and
        // both silently stopped covering the top of the range when this file
        // grew (GLStateGuard's own comment records it drifting twice before
        // that). A hand-picked name has to be MOVED on every addition; this
        // one only has to be RAISED when a binding exceeds it, which the
        // static_assert below makes a compile error rather than a black frame.
        static constexpr u32 UBO_BINDING_LIMIT = 83;
        static_assert(UBO_WATER_DISTURBANCE < UBO_BINDING_LIMIT &&
                          UBO_AUTO_EXPOSURE < UBO_BINDING_LIMIT && UBO_INSTANCE_CULL < UBO_BINDING_LIMIT &&
                          UBO_REFLECTION_PROBE_CULL < UBO_BINDING_LIMIT &&
                          UBO_REFLECTION_PROBES < UBO_BINDING_LIMIT && UBO_HEAP_OFFSETS < UBO_BINDING_LIMIT &&
                          UBO_DEBUG_DRAW < UBO_BINDING_LIMIT && UBO_PRECIPITATION_FEED < UBO_BINDING_LIMIT &&
                          UBO_COLORBLIND < UBO_BINDING_LIMIT && UBO_PREFIX_SUM < UBO_BINDING_LIMIT &&
                          UBO_TERRAIN_CULL < UBO_BINDING_LIMIT &&
                          UBO_VIRTUAL_SHADOW < UBO_BINDING_LIMIT &&
                          UBO_VIRTUAL_SHADOW_DRAW < UBO_BINDING_LIMIT,
                      "UBO_BINDING_LIMIT must stay one past the highest engine UBO binding");
        // GL 4.6's MINIMUM guarantee for GL_MAX_UNIFORM_BUFFER_BINDINGS — the floor every
        // conforming implementation must meet. It is a COUNT, so it is an EXCLUSIVE upper bound:
        // the portable binding indices are 0..83, and 84 itself must never be handed out. Compare
        // against it with `<` (an array sized by it, like the bind-state mirror's, makes that
        // automatic); `<=` is only correct for a value that is itself a count, which is why the
        // UBO_BINDING_LIMIT assert above uses one. Backend-neutral on purpose: it is a GL-derived
        // bound that the Vulkan bind-state mirror also sizes its arrays to
        // (VulkanBindingState::kMaxBufferBindings aliases this), and it used to be spelled as a
        // bare 84 in both places. Naming it here rather than there is what lets a GL-only test
        // assert against it — VirtualShadowMapLocalTest reached into
        // Platform/Vulkan/VulkanBindingState.h for the constant and stopped compiling under
        // OLO_WITH_VULKAN=OFF, which nothing built until #811 added the CI job.
        static constexpr u32 MIN_GUARANTEED_BUFFER_BINDINGS = 84;
        static_assert(UBO_BINDING_LIMIT <= MIN_GUARANTEED_BUFFER_BINDINGS,
                      "Engine UBO binding points exceed the GL 4.6 minimum GL_MAX_UNIFORM_BUFFER_BINDINGS");

        // =============================================================================
        // TEXTURE SAMPLER BINDINGS
        // =============================================================================

        static constexpr u32 TEX_DIFFUSE = 0;     // Primary diffuse/albedo texture
        static constexpr u32 TEX_SPECULAR = 1;    // Specular/metallic texture
        static constexpr u32 TEX_NORMAL = 2;      // Normal map
        static constexpr u32 TEX_HEIGHT = 3;      // Height/displacement map
        static constexpr u32 TEX_AMBIENT = 4;     // Ambient occlusion
        static constexpr u32 TEX_EMISSIVE = 5;    // Emissive map
        static constexpr u32 TEX_ROUGHNESS = 6;   // Roughness map
        static constexpr u32 TEX_METALLIC = 7;    // Metallic map
        static constexpr u32 TEX_SHADOW = 8;      // Shadow map (CSM, sampler2DArrayShadow)
        static constexpr u32 TEX_ENVIRONMENT = 9; // Environment/skybox
        static constexpr u32 TEX_USER_0 = 10;     // User-defined texture 0
        static constexpr u32 TEX_USER_1 = 11;     // User-defined texture 1
        static constexpr u32 TEX_USER_2 = 12;     // User-defined texture 2
        // The budgeted local-light shadow atlas (issue #435) — a 1-layer depth
        // array holding every spot / point-face shadow tile. Replaces the old
        // 4-layer spot array on this slot; of the four point-cubemap slots
        // 14-17 it also freed, 14/15 now carry the reflection-probe cubemap
        // arrays (issue #705), 16 the scene lightmap atlas (issue #439), and
        // 17 the shared blue-noise tile (issue #706, declared below).
        static constexpr u32 TEX_SHADOW_ATLAS = 13; // Local-light shadow atlas (sampler2DArrayShadow, 1 layer)
        // Distance-impostor reflection probes (issue #705): every baked
        // probe's prefiltered radiance chain and R32F radial-distance field,
        // one array layer per probe (samplerCubeArray). Published by
        // ReflectionProbeArray::BindForShading; consumed by the deferred and
        // forward lit passes via include/ReflectionProbes.glsl.
        static constexpr u32 TEX_REFLECTION_PROBE_RADIANCE = 14; // RGBA32F prefilter chains (roughness mips)
        static constexpr u32 TEX_REFLECTION_PROBE_DISTANCE = 15; // R32F radial distance + max-mips
        // Scene lightmap atlas (issue #439): baked indirect irradiance E in the
        // reference tracer's physical units (the DDGI atlas convention), one
        // RGBA16F texture for the whole scene. Per-instance atlas regions ride
        // InstanceData::LightmapScaleOffset; the parameters block is UBO_LIGHTMAP.
        // Bound through the heap-bindless seam (PBR_MultiLight is a converted
        // program), consumed via include/LightmapSampling.glsl.
        static constexpr u32 TEX_LIGHTMAP = 16; // Scene lightmap atlas (sampler2DArray, RGBA16F, one layer per page — issue #868)
        // The shared screen-space blue-noise tile (issue #706): 64x64 RG8, two
        // channels of independent void-and-cluster noise generated on the CPU by
        // Renderer/BlueNoise.h and sampled through
        // include/StochasticCommon.glsl. Nearest-filtered, repeat-wrapped; the
        // shader wraps with a bitmask, so the sampler wrap mode is belt-and-braces.
        //
        // Took slot 17 — free in the SAMPLER namespace since issue #435 retired
        // the point-cubemap slots — deliberately, so this costs no renumbering of
        // TEX_SHADER_GRAPH_0 and therefore does not move MAX_ENGINE_TEXTURE_SLOTS
        // or HEAP_IMAGE_SLOT_BASE. That derived-base drift is what issue #702
        // caused and BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout
        // now guards; not triggering it at all beats triggering it correctly.
        //
        // 17 IS claimed in the other two namespaces (UBO_FOG,
        // SSBO_INSTANCE_DRAW_INDIRECT). Legal on GL's disjoint namespaces and on
        // Vulkan too, EXCEPT within one shader — the ADR item A2 collision that
        // moved TEX_DDGI_VISIBILITY off 57. So the standing constraint is: no
        // shader may sample this slot AND declare uniform/storage block 17.
        // Checked by StochasticSamplerTest.NoShaderCollidesWithTheBlueNoiseSlot,
        // which parses the shader tree rather than trusting this comment.
        static constexpr u32 TEX_BLUE_NOISE = 17;           // Shared blue-noise tile (sampler2D, RG8, 64x64)
        static constexpr u32 TEX_POSTPROCESS_LUT = 18;      // Post-process color grading LUT
        static constexpr u32 TEX_POSTPROCESS_DEPTH = 19;    // Post-process scene depth access
        static constexpr u32 TEX_SSAO = 20;                 // Blurred SSAO result
        static constexpr u32 TEX_SSAO_NOISE = 21;           // SSAO 4x4 rotation noise texture
        static constexpr u32 TEX_SCENE_NORMALS = 22;        // View-space normals from G-buffer
        static constexpr u32 TEX_TERRAIN_HEIGHTMAP = 23;    // Terrain heightmap (R32F)
        static constexpr u32 TEX_TERRAIN_SPLATMAP = 24;     // Terrain splatmap 0 (RGBA8, layers 0-3)
        static constexpr u32 TEX_TERRAIN_ALBEDO_ARRAY = 25; // Terrain albedo layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_NORMAL_ARRAY = 26; // Terrain normal map layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_ARM_ARRAY = 27;    // Terrain ARM layer array (Texture2DArray)
        static constexpr u32 TEX_TERRAIN_SPLATMAP_1 = 28;   // Terrain splatmap 1 (RGBA8, layers 4-7)
        static constexpr u32 TEX_WIND_FIELD = 29;           // 3D wind velocity field (sampler3D, RGBA16F)
        static constexpr u32 TEX_SNOW_DEPTH = 30;           // Snow accumulation depth map (sampler2D, R32F)
        static constexpr u32 TEX_PRECIPITATION_NOISE = 31;  // Precipitation streak/lens noise (sampler2D)
        // Nearest water-surface depth (DEPTH_COMPONENT32F) captured by WaterRenderPass;
        // sampled by the underwater-fog stage in the ToneMap pass to find the per-pixel
        // wavy water boundary (WATER_FUTURE_IMPROVEMENTS.md §7.2). Took GTAO-reserved
        // slot 32 — GTAO binds low sequential slots (0-5) instead, so 33-35 stay free.
        static constexpr u32 TEX_UNDERWATER_WATER_DEPTH = 32;
        // Comparison-OFF raw-depth views of the CSM array / shadow atlas, bound as
        // plain sampler2DArray so the PCSS blocker search can read raw occluder
        // depth (the hardware sampler2DArrayShadow at TEX_SHADOW / TEX_SHADOW_ATLAS
        // only yields the comparison result). Took the formerly GTAO-reserved
        // slots 33-34 (GTAO binds low sequential slots 0-5 instead).
        static constexpr u32 TEX_SHADOW_CSM_RAW = 33;   // Raw-depth view of the CSM array (PCSS blocker search)
        static constexpr u32 TEX_SHADOW_ATLAS_RAW = 34; // Raw-depth view of the shadow atlas (PCSS blocker search)
        static constexpr u32 TEX_SSR_HZB = 35;          // Min-depth HZB pyramid for HiZ-accelerated SSR traversal (#284)
        static constexpr u32 TEX_WATER_NORMAL_0 = 36;   // Water scrolling normal map 0
        static constexpr u32 TEX_WATER_NORMAL_1 = 37;   // Water scrolling normal map 1
        static constexpr u32 TEX_WATER_NOISE = 38;      // Water specular noise texture
        static constexpr u32 TEX_WATER_DEPTH = 39;      // Scene depth for water depth effects
        static constexpr u32 TEX_WATER_REFRACTION = 40; // Pre-water scene color for refraction
        static constexpr u32 TEX_WATER_FOAM = 41;       // Foam texture
        static constexpr u32 TEX_WATER_SSR = 42;        // SSR reflection result for water
        // Deferred renderer G-Buffer sampler slots (consumed by DeferredLightingPass).
        // RT0: Albedo (RGB) + Metallic (A) — RGBA8
        // RT1: Octahedral Normal (RG) + Roughness + AO — RGBA16F (packed)
        // RT2: Emissive (RGB) + Material flags (A) — RGBA16F
        // RT3: Velocity (RG) — RG16F
        // RT5: Baked lightmap irradiance (RGB) + coverage (A) — RGBA16F (issue #865)
        static constexpr u32 TEX_GBUFFER_ALBEDO = 43;   // G-Buffer RT0 (albedo + metallic)
        static constexpr u32 TEX_GBUFFER_NORMAL = 44;   // G-Buffer RT1 (normal + roughness + AO)
        static constexpr u32 TEX_GBUFFER_EMISSIVE = 45; // G-Buffer RT2 (emissive + flags)
        static constexpr u32 TEX_GBUFFER_VELOCITY = 46; // G-Buffer RT3 (velocity)
        static constexpr u32 TEX_GBUFFER_DEPTH = 47;    // G-Buffer depth attachment
        // Weighted-blended OIT accumulation targets. Sampled by
        // OIT_Resolve.glsl; written to (not sampled) by transparent passes
        // when RendererSettings::OITEnabled is on (path-agnostic).
        static constexpr u32 TEX_OIT_ACCUM = 48;     // OIT accum buffer (RGBA16F: sum(Ci*ai*wi), sum(ai*wi))
        static constexpr u32 TEX_OIT_REVEALAGE = 49; // OIT revealage buffer (R16F: prod(1 - ai))
        // FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1). Sampled by
        // Water.glsl when the surface is in FFT mode (rgb = choppy displacement,
        // a = foam; and rgb = normal, a = Jacobian respectively).
        static constexpr u32 TEX_WATER_FFT_DISPLACEMENT = 50; // dx, height, dz, foam
        static constexpr u32 TEX_WATER_FFT_DERIVATIVES = 51;  // normal.xyz, jacobian
        // Planar-reflection color target (the opaque scene re-rendered from the
        // mirrored camera) sampled projectively by Water.glsl. Sits right after
        // the FFT cascade slots so all water inputs stay contiguous; the
        // shader-graph user base shifts up by one to keep "after engine slots".
        static constexpr u32 TEX_WATER_PLANAR_REFLECTION = 52;
        // Integrated froxel fog volume (sampler3D, RGBA16F: rgb = accumulated
        // in-scatter, a = transmittance) written by the VolumetricFogPass
        // compute chain and sampled by PostProcess_Fog.glsl (issue #435). The
        // shader-graph user base shifts up by one, per the established
        // procedure for new engine slots.
        static constexpr u32 TEX_FROXEL_FOG = 53;
        // Screen-space fluid intermediates (issue #630): the bilateral-smoothed
        // view-space depth and the additive thickness/foam accumulation target,
        // written by the fluid depth/smooth/thickness passes and sampled by the
        // fluid composite. The shader-graph user base shifts up by two, per the
        // established procedure for new engine slots.
        static constexpr u32 TEX_FLUID_DEPTH = 54;     // R32F view-space depth (smoothed)
        static constexpr u32 TEX_FLUID_THICKNESS = 55; // RG16F: r = thickness, g = speed-weighted thickness (foam)
        // Realtime DDGI probe atlases (issue #632), written by the DDGI update
        // passes and sampled by the deferred/forward lit passes via
        // include/DDGICommon.glsl (and exposed as the shared binding a future
        // froxel-fog bounce term can sample).
        static constexpr u32 TEX_DDGI_IRRADIANCE = 56; // RGBA16F octahedral irradiance atlas (6x6 interior + 1px border per probe)
        // TEX_DDGI_VISIBILITY was 57 and is now 64 (issue #691,
        // ADR item A2): 57 was triple-booked across GL's disjoint namespaces
        // (UBO_DEBUG_DRAW = 57 + this sampler + the SSBO_VERTEX_PULL stream
        // below), and DDGI_Capture.glsl is the first shader that PULLS its
        // vertices (SSBO 57) while also including DDGICommon.glsl (sampler 57)
        // — on Vulkan's single-set model that is a real within-shader
        // collision, unlike the UBO (DebugDrawPrimitives never pulls).
        // 64 was outside every namespace's used range; the shader-graph user
        // base below shifts up past it, per the established procedure for new
        // engine slots. Mirrors include/DDGICommon.glsl's global-sampler block.
        static constexpr u32 TEX_DDGI_VISIBILITY = 64; // RG16F Chebyshev atlas (mean, mean^2 distance; 14x14 interior + 1px border)
        static constexpr u32 TEX_DDGI_PROBE_DATA = 58; // RGBA16F per-probe data (xyz = relocation offset / spacing, w = state)
        // Volumetric cloudscape inputs (issue #633): tiling Perlin-Worley 3D
        // noise (base 128³ + detail 32³, RGBA8, repeat), the 2D weather map
        // (R = coverage, G = type, B = wetness), and the top-down cloud-shadow
        // transmittance map sampled by the PBR direct-light path. Together with
        // the DDGI atlases above, the shader-graph user base shifts up by
        // seven, per the established procedure for new engine slots.
        static constexpr u32 TEX_CLOUD_BASE_NOISE = 59;   // sampler3D RGBA8 (repeat)
        static constexpr u32 TEX_CLOUD_DETAIL_NOISE = 60; // sampler3D RGBA8 (repeat)
        static constexpr u32 TEX_CLOUD_WEATHER_MAP = 61;  // sampler2D RGBA8
        static constexpr u32 TEX_CLOUD_SHADOW = 62;       // sampler2D R8 (1 = unshadowed transmittance)
        // 63 is deliberately left unused in the SAMPLER namespace: it is
        // SSBO_BONE_PULL in the storage namespace (see below), and keeping the
        // number sampler-free means no shader can ever recreate the A2
        // collision on Vulkan's single-set model. The shader-graph base sits
        // past TEX_DDGI_VISIBILITY (64), per the established shift procedure.
        //
        // The Virtual Shadow Map physical page pool (issue #702). 65, NOT 57 — 57
        // is free in the sampler namespace today, but it is also SSBO_VERTEX_PULL,
        // and include/VirtualShadowSampling.glsl is included by shaders that DO
        // pull their vertices (the deferred-lighting fullscreen pass among them),
        // which is exactly the within-shader collision the A2 renumber above
        // exists to prevent.
        static constexpr u32 TEX_VSM_PHYSICAL = 65; // usampler2D R32UI — raw float depth bits, point-sampled
        // The shared volumetric shadow volume (issue #723): a light-space
        // sampler3D R32F holding the optical depth accumulated FROM the light
        // through the participating media, as two stacked cascades — cloud
        // layer (slices [0, kSlices)) and fog (slices [kSlices, 2*kSlices)).
        // Written by compute/VolumetricShadow_Generate.comp, read by the cloud
        // raymarch and the froxel fog scatter through
        // include/VolumetricShadowCommon.glsl. GLSL twin: u_VolumetricShadowVolume.
        static constexpr u32 TEX_VOLUMETRIC_SHADOW = 66; // sampler3D R32F — optical depth from the light (2 stacked cascades)
        // Moved 65 -> 66 for TEX_VSM_PHYSICAL, then 66 -> 67 for
        // TEX_VOLUMETRIC_SHADOW, per the established shift procedure.
        // Terrain adaptive virtual texturing (issue #715). GLSL twin:
        // include/TerrainVirtualTexture.glsl, which is the ONLY place either is
        // declared.
        //
        // The cache is a two-LAYER array rather than two 2D textures, and that
        // is a slot-budget decision made deliberately: every texture slot added
        // here shifts HEAP_IMAGE_SLOT_BASE, and that base is mirrored by a
        // hand-written literal in BindlessHeap.glsl (see the storage-image
        // section below for the #702 incident). One array costs one slot and one
        // layered image binding instead of two of each.
        //   layer 0 = albedo.rgb + AO, layer 1 = normal.xy + roughness + metallic
        static constexpr u32 TEX_TERRAIN_VT_INDIRECTION = 67; // RGBA8 + mip chain: virtual page -> physical tile
        static constexpr u32 TEX_TERRAIN_VT_CACHE = 68;       // RGBA8 2-layer physical cache atlas

        // Deferred baked-GI G-Buffer target (issue #865): the sixth G-Buffer
        // attachment, holding the baked lightmap irradiance E sampled during the
        // G-Buffer pass plus its coverage in .a. Numbered here rather than next to
        // TEX_GBUFFER_DEPTH because 48..68 are all taken; the G-Buffer slots are a
        // semantic group, not a contiguous range. The shader-graph user base below
        // shifts up by one, per the established procedure for new engine slots —
        // which also moves HEAP_IMAGE_SLOT_BASE, so
        // include/BindlessHeap.glsl's OLO_HEAP_IMAGE_BASE literal (and
        // BindlessHeapGpuTest.cpp's inline copy) move with it.
        static constexpr u32 TEX_GBUFFER_BAKEDGI = 69; // G-Buffer RT5 (baked lightmap irradiance + coverage)

        // Boat / actor wake foam (issue #967). RG16F; only .r is read, .g is
        // reserved. World-anchored and toroidally stored — see
        // Renderer/Water/WaterDisturbanceField.h for the addressing, and note
        // that this is the SAMPLED view; the compute writes the same texture
        // through storage image unit 0.
        static constexpr u32 TEX_WATER_DISTURBANCE = 70;

        static constexpr u32 TEX_SHADER_GRAPH_0 = 71; // First shader graph user texture slot (must be after all engine-reserved slots)

        // Tracker capacity for CommandDispatchData::BoundTextureIDs. Must be
        // strictly greater than the highest engine-reserved slot so redundant-
        // bind tracking writes never go out of bounds. Grows with any new
        // TEX_* constant added above.
        static constexpr u32 MAX_ENGINE_TEXTURE_SLOTS = TEX_SHADER_GRAPH_0 + 1;

        // Ensure all engine-reserved texture slots fit within the GL 4.6 minimum guarantee (80 combined units).
        static_assert(TEX_SHADER_GRAPH_0 < 80, "Engine texture slots exceed GL 4.6 minimum GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS");

        // =============================================================================
        // STORAGE-IMAGE (imageLoad / imageStore) BINDINGS — issue #691
        //
        // Image units are a SEPARATE GL namespace from texture units:
        // glBindImageTexture(unit, ...) and glBindTextureUnit(slot, ...) both start
        // at 0 and mean different things. The heap-offset table has only ONE index
        // space, so an image binding at unit 0 and a texture binding at TEX_DIFFUSE
        // would collide and each would silently overwrite the other's offset.
        //
        // Image unit `u` therefore occupies table index HEAP_IMAGE_SLOT_BASE + u.
        // The base is applied on both sides — by RGCommandContext::BindImageOrHeapOffset
        // on the CPU and by include/BindlessHeap.glsl's OLO_HEAP_IMAGE_* macros in
        // the shader — so the shader names the SAME image unit the bind names.
        // That is ADR 0011 amendment (25)'s property preserved across the second
        // descriptor kind.
        //
        // ⚠ THE TWO SIDES CAN DISAGREE, AND HAVE. This comment used to claim they
        // could not "because both derive from this one constant". Only the C++
        // side does: `OLO_HEAP_IMAGE_BASE` in BindlessHeap.glsl is a hand-written
        // literal. And this base is DERIVED — it is MAX_ENGINE_TEXTURE_SLOTS — so
        // it MOVES the moment anyone adds a TEX_* slot above.
        //
        // Issue #702 did exactly that (TEX_VSM_PHYSICAL at 65), shifting the base
        // 66 → 67 while the GLSL literal stayed at 66. Every bindless storage image
        // then resolved one index low and read a SAMPLER descriptor through an
        // image declaration — undefined behaviour, not a blank read.
        //
        // SO: ADDING A TEXTURE SLOT IS ALSO A SHADER EDIT. Update
        // `OLO_HEAP_IMAGE_BASE` in OloEditor/assets/shaders/include/BindlessHeap.glsl
        // (and the copy in BindlessHeapGpuTest.cpp's inline prologue) in the same
        // commit. Both are pinned headlessly by
        // BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout, so the drift
        // now fails a test on any machine instead of only a bindless-capable GPU.
        static constexpr u32 HEAP_IMAGE_SLOT_BASE = MAX_ENGINE_TEXTURE_SLOTS;

        // GL 4.6 guarantees GL_MAX_IMAGE_UNITS >= 8. The engine's deepest user is
        // HZBGenerator's 4-mip batch; nothing binds above unit 3.
        static constexpr u32 MAX_ENGINE_IMAGE_SLOTS = 8;

        // Total entries in the shared heap-offset table: every texture slot, then
        // every image slot, rounded UP to a whole uvec4 group (68 + 8 = 76 used
        // entries → 76 declared; the A2 renumber moved the base off a multiple
        // of four). The round-up is why #702's extra texture slot did NOT change
        // this number — 74 and 75 both round to 76 — which is exactly why the
        // image-base drift it caused went unnoticed: the shader's array size, the
        // more obvious of the two mirrors, still matched. #723's TEX_VOLUMETRIC_SHADOW
        // is the third slot to land in that same blind spot (75 → 76 still rounds
        // to 76), so the OLO_HEAP_IMAGE_BASE edit below is again the only visible
        // half of the change. The trailing pad entries are never indexed — image units
        // stay < MAX_ENGINE_IMAGE_SLOTS — they only keep the std140 block a
        // whole number of uvec4s. include/BindlessHeap.glsl declares
        // `uvec4 g_OloHeapOffsets[HEAP_OFFSET_TABLE_VEC4S]` and must match.
        static constexpr u32 HEAP_OFFSET_TABLE_SLOTS =
            ((HEAP_IMAGE_SLOT_BASE + MAX_ENGINE_IMAGE_SLOTS + 3u) / 4u) * 4u;
        static constexpr u32 HEAP_OFFSET_TABLE_VEC4S = HEAP_OFFSET_TABLE_SLOTS / 4u;
        static_assert(HEAP_OFFSET_TABLE_VEC4S * 4u == HEAP_OFFSET_TABLE_SLOTS,
                      "The heap-offset table must be a whole number of uvec4s — std140 pads a uint array to a "
                      "16-byte stride, so a partial group would put the last slots outside the block the shader "
                      "declares.");

        // =============================================================================
        // SHADER STORAGE BUFFER OBJECT (SSBO) BINDINGS
        // =============================================================================

        static constexpr u32 SSBO_GPU_PARTICLES = 0;     // GPU particle data array
        static constexpr u32 SSBO_ALIVE_INDICES = 1;     // Compacted alive particle index buffer
        static constexpr u32 SSBO_COUNTERS = 2;          // Atomic counters (alive/dead/emit counts)
        static constexpr u32 SSBO_FREE_LIST = 3;         // Free-slot indices for emission recycling
        static constexpr u32 SSBO_INDIRECT_DRAW = 4;     // Indirect draw command buffer
        static constexpr u32 SSBO_EMIT_STAGING = 5;      // Staging buffer for newly emitted particles
        static constexpr u32 SSBO_FOLIAGE_INSTANCES = 6; // Foliage instance data (reserved for GPU-driven path)
        static constexpr u32 SSBO_SNOW_DEFORMERS = 7;    // Snow deformer stamp data (pos, radius, depth)
        static constexpr u32 SSBO_LIGHT_PROBES = 8;      // Light probe SH coefficient data

        // Forward+ light culling SSBOs
        static constexpr u32 SSBO_FPLUS_POINT_LIGHTS = 9;        // Forward+ point light data array
        static constexpr u32 SSBO_FPLUS_SPOT_LIGHTS = 10;        // Forward+ spot light data array
        static constexpr u32 SSBO_FPLUS_LIGHT_INDICES = 11;      // Forward+ per-tile light index list
        static constexpr u32 SSBO_FPLUS_LIGHT_GRID = 12;         // Forward+ per-tile (offset, count) pairs
        static constexpr u32 SSBO_FPLUS_GLOBAL_INDEX = 13;       // Forward+ atomic counter for light index append
        static constexpr u32 SSBO_GPU_PARTICLES_PREV = 14;       // GPU particle previous-frame positions (vec4[maxParticles]) for motion vectors
        static constexpr u32 SSBO_INSTANCE_DATA = 15;            // Per-instance transform/color/entity data indexed by gl_InstanceIndex
        static constexpr u32 SSBO_INSTANCE_CULL_INPUT = 16;      // Full InstanceData[] input to the GPU frustum-cull compute (output goes back to SSBO_INSTANCE_DATA)
        static constexpr u32 SSBO_INSTANCE_DRAW_INDIRECT = 17;   // DrawElementsIndirectCommand populated by the cull compute, read by glDrawElementsIndirect
        static constexpr u32 SSBO_FPLUS_SPHERE_AREA_LIGHTS = 18; // Forward+ sphere area light data array (Karis 2013 representative-point)

        // Automatic exposure / eye adaptation (histogram metering -> ToneMap)
        static constexpr u32 SSBO_AUTO_EXPOSURE_HISTOGRAM = 19; // 256-bin log-luminance histogram (AutoExposureHistogram.comp -> AutoExposureAverage.comp)
        static constexpr u32 SSBO_AUTO_EXPOSURE_STATE = 20;     // [0]=exposure multiplier (<=0 => use manual), [1]=adapted luminance (persists across frames)

        // GPU fluid solver (Position-Based Fluids, issue #630) — Fluid_*.comp
        static constexpr u32 SSBO_FLUID_POSITIONS = 21;      // vec4[max]: xyz world position, w = kill flag (< 0 marked dead)
        static constexpr u32 SSBO_FLUID_VELOCITIES = 22;     // vec4[max]: xyz velocity
        static constexpr u32 SSBO_FLUID_PREDICTED_A = 23;    // vec4[max]: predicted positions, Jacobi ping
        static constexpr u32 SSBO_FLUID_PREDICTED_B = 24;    // vec4[max]: predicted positions, Jacobi pong
        static constexpr u32 SSBO_FLUID_AUX = 25;            // vec4[max]: xyz vorticity omega, w lambda
        static constexpr u32 SSBO_FLUID_GRID_HEAD = 26;      // u32[cells]: neighbour-grid linked-list heads (index+1, 0 = empty)
        static constexpr u32 SSBO_FLUID_GRID_NEXT = 27;      // u32[max]: neighbour-grid linked-list next pointers (index+1)
        static constexpr u32 SSBO_FLUID_COUNTERS = 28;       // GPUFluidCounters (count/emit/kill/scratch)
        static constexpr u32 SSBO_FLUID_EMIT_STAGING = 29;   // GPUFluidEmitEntry[batch]: CPU-staged emissions
        static constexpr u32 SSBO_FLUID_BODY_PROXIES = 30;   // FluidBodyProxy[kFluidMaxBodyProxies]: Jolt coupling shapes (CPU-uploaded)
        static constexpr u32 SSBO_FLUID_BODY_IMPULSES = 31;  // GPUFluidBodyImpulse[kFluidMaxBodyProxies]: fixed-point reaction accumulators
        static constexpr u32 SSBO_FLUID_VELOCITIES_ALT = 32; // vec4[max]: XSPH/vorticity-corrected velocities (pong)

        // Virtualized geometry cluster pipeline (Nanite-style cluster LOD DAG, issue #629)
        // — VirtualClusterCull.comp / VirtualMeshGBuffer.glsl
        static constexpr u32 SSBO_VIRTUAL_CLUSTERS = 33;      // VirtualClusterGpuRecord[]: cull spheres + cones + geometry windows (all registered meshes pooled)
        static constexpr u32 SSBO_VIRTUAL_GROUPS = 34;        // VirtualGroupGpuRecord[]: LOD spheres + monotone DAG errors
        static constexpr u32 SSBO_VIRTUAL_INSTANCES = 35;     // VirtualInstanceGpuRecord[]: per-frame instance transforms + mesh ranges
        static constexpr u32 SSBO_VIRTUAL_DRAW_COMMANDS = 36; // DrawElementsIndirectCommand[]: compacted by the cull compute, segmented per instance
        static constexpr u32 SSBO_VIRTUAL_DRAW_ARGS = 37;     // VirtualDrawArgs[instance]: draw count (also bound as GL_PARAMETER_BUFFER) + cull stats
        static constexpr u32 SSBO_VIRTUAL_VISIBLE = 38;       // VirtualVisibleCluster[]: per-draw (instance, cluster) records indexed via gl_BaseInstance
        static constexpr u32 SSBO_VIRTUAL_VERTICES = 39;      // VirtualGpuVertex[]: cluster-owned packed vertices (positionU, normalV)
        static constexpr u32 SSBO_VIRTUAL_SW_LIST = 40;       // { uint Count; pad[3]; VirtualVisibleCluster[] }: clusters routed to the software rasterizer
        static constexpr u32 SSBO_VIRTUAL_VISBUFFER = 41;     // uvec2[width*height] visibility buffer: .y = depth bits (atomicMin), .x = (visibleSlot << 9 | tri)
        static constexpr u32 SSBO_VIRTUAL_INDICES = 42;       // u32[]: pooled cluster-local index buffer (same GL buffer the MDI path uses as element array)
        static constexpr u32 SSBO_VIRTUAL_GROUP_STATES = 43;  // u32[group]: bit0 = page resident (CPU), bit1 = page requested (GPU atomicOr), bit2 = touched for LRU (GPU atomicOr)
        static constexpr u32 SSBO_VIRTUAL_REJECTED = 44;      // { uint Count; pad[3]; VirtualVisibleCluster[] }: clusters the two-phase cull's phase 1 found hidden by the PREVIOUS frame's Hi-Z, re-tested by phase 2 (issue #682)
        // The shader-visible descriptor heap (issue #691). uvec2[] of
        // ARB_bindless_texture handles, indexed by an RHI::HeapOffset that
        // travels to the shader as ordinary UBO data. This is the binding that
        // replaces BINDING ITSELF: under heap-bindless a pass writes an offset
        // instead of calling BindTexture, and the TEX_* constants above survive
        // only as the numbers, promoted from compile-time constants to runtime
        // data (ADR 0011 §1.1). Bound once per frame, never per draw.
        static constexpr u32 SSBO_RESOURCE_HEAP = 45;

        // GPU-pushable shader debug draws (issue #725) — one append channel per
        // primitive type, declared by include/DebugDrawCommon.glsl and drawn by
        // assets/shaders/DebugDrawPrimitives.glsl. Every channel has the SAME
        // 32-byte header (a DrawArraysIndirectCommand followed by Capacity +
        // RequestCount) and differs only in its trailing entry array, so the C++
        // mirror in Renderer/Debug/ShaderDebugDrawTypes.h can describe all seven
        // with one header struct. The channel buffer is bound BOTH as an SSBO
        // here and as the GL_DRAW_INDIRECT_BUFFER of its own draw — the args live
        // at offset 0 precisely so `glDrawArraysIndirect(.., nullptr)` finds them.
        //
        // The order of these seven MUST match ShaderDebugDrawPrimitive's
        // enumerator order (pinned by ShaderDebugDrawContractTest) — the C++ side
        // indexes its channel array by the enumerator and derives the binding as
        // SSBO_DEBUG_DRAW_FIRST + index.
        static constexpr u32 SSBO_DEBUG_DRAW_LINE = 46;
        static constexpr u32 SSBO_DEBUG_DRAW_CIRCLE = 47;
        static constexpr u32 SSBO_DEBUG_DRAW_RECTANGLE = 48;
        static constexpr u32 SSBO_DEBUG_DRAW_AABB = 49;
        static constexpr u32 SSBO_DEBUG_DRAW_BOX = 50;
        static constexpr u32 SSBO_DEBUG_DRAW_CONE = 51;
        static constexpr u32 SSBO_DEBUG_DRAW_SPHERE = 52;

        static constexpr u32 SSBO_DEBUG_DRAW_FIRST = SSBO_DEBUG_DRAW_LINE;
        static constexpr u32 SSBO_DEBUG_DRAW_COUNT = 7;

        // Distance-impostor reflection probes (issue #705): one u32 bitmask
        // per froxel cluster (bit i = probe i's influence sphere overlaps the
        // cluster), written by compute/ReflectionProbeCull.comp, read by the
        // lit passes via include/ReflectionProbes.glsl. Same 32x18x24 grid
        // and slice mapping as the Forward+ light grid.
        static constexpr u32 SSBO_REFLECTION_PROBE_GRID = 53;

        // Virtual Shadow Maps (issue #702) — the GPU-driven page allocator's whole
        // working set. GLSL twin: include/VirtualShadowBuffers.glsl (plus
        // VirtualShadowResources.glsl for the page table and
        // VirtualShadowDrawList.glsl for the cull output, which the depth raster
        // reads from a vertex stage).
        //
        // SSBOs rather than R32UI images throughout, and that is forced rather
        // than preferred: the reference implementation's meta table is R64_UINT
        // and needs int64 IMAGE atomics, which are an extension in GL 4.6.
        // Directional-only page ownership fits in 32 bits, and SSBO atomics are
        // core — so the allocator runs on guaranteed functionality. Only the
        // physical pool stays a texture, because the raster writes it with
        // imageAtomicMin and the lit pass samples it.
        //
        // 57 and 63 are skipped: both are reserved engine-wide for the Vulkan
        // vertex-pull streams (see below).
        //
        // 68..77, the THIRD numbering in one PR: #713's prefix sum owns 54..56
        // and #714's terrain cull owns 58..62 and 65..67 — both landed on master
        // while this branch was in flight, and both collisions would have been
        // quietly wrong rather than loud: those systems rebind per dispatch,
        // whereas VSM binds its ten ONCE in BindWorkingSet() and then runs nine
        // kernels off them, so any foreign dispatch in between would swap the
        // page table out from under a live frame. Now contiguous — the range
        // sits entirely above the reserved vertex-pull (57) / bone-pull (63)
        // slots, so no skip is needed — and bounded by kMaxBufferBindings (84)
        // via the reflection test's kHighestKnownSSBOBinding.
        static constexpr u32 SSBO_VSM_PAGE_TABLE = 68;     // virtual page -> flags + physical page
        static constexpr u32 SSBO_VSM_META_TABLE = 69;     // physical page -> owning virtual page
        static constexpr u32 SSBO_VSM_HPB = 70;            // dirty-flag pyramid, 7 mips x 16 clip levels
        static constexpr u32 SSBO_VSM_REQUESTS = 71;       // allocation requests appended by the marker
        static constexpr u32 SSBO_VSM_FREE_PAGES = 72;     // free list + evictable (not-visited) list
        static constexpr u32 SSBO_VSM_INVALIDATIONS = 73;  // dynamic-caster bounds to re-dirty
        static constexpr u32 SSBO_VSM_CULL_INSTANCES = 74; // cull input, one per submitted caster
        static constexpr u32 SSBO_VSM_DRAW_INSTANCES = 75; // cull output, one per (caster x clip level)
        static constexpr u32 SSBO_VSM_DRAW_COMMANDS = 76;  // DrawElementsIndirectCommand per caster batch
        static constexpr u32 SSBO_VSM_STATS = 77;          // page counters, read back one frame late

        // The ONE binding issue #703 (local-light virtual shadow pages) had to
        // add, and it is called out here because the space is nearly gone: the
        // UBO namespace has ONE slot left under the GL 4.6 minimum of 84
        // (UBO_BINDING_LIMIT is 83), and two separate PRs already collided in
        // this SSBO range mid-flight, once SILENTLY (#818's overlap compiled
        // side by side and only the runtime would have noticed).
        //
        // Everything else #703 needed rode an existing buffer — the local pages
        // are appended to SSBO_VSM_PAGE_TABLE, the local pyramids to
        // SSBO_VSM_HPB, the local requests share SSBO_VSM_REQUESTS with a bit in
        // the record, and the local draws share SSBO_VSM_DRAW_INSTANCES /
        // _COMMANDS with disjoint ranges. This one could not: the per-layer
        // projections are ~40 KB, past the GL 4.6 minimum UBO size (16 KB), so
        // they cannot live in a uniform block, and no existing SSBO holds
        // anything with a compatible lifetime.
        //
        // Layout note for the next reader: the block has a FIXED header (three
        // uint[256] arrays) before its unsized tail, because std430 allows only
        // the last member to be unsized. GLSL twin: the VSMLocalLights block in
        // include/VirtualShadowResources.glsl.
        static constexpr u32 SSBO_VSM_LOCAL_LIGHTS = 78; // per-layer projections + per-layer frame state

        // Realtime DDGI sparsity + GPU relocation (issue #707). TWO storage
        // bindings, and ZERO new UBO bindings — the per-cascade lattice arrays
        // ride inside the existing DDGIVolumeUBO (binding 51) at 512 B, which
        // is what keeps the one remaining UBO slot free for whoever needs it
        // next. Read the SSBO_VSM_LOCAL_LIGHTS note above before adding more.
        //
        // SSBO_DDGI_PROBE_AUX is one record per probe across ALL cascades: the
        // last frame a shaded pixel (or another probe's hit point) requested it,
        // its GPU-computed classification, and the #751 bounce-coverage
        // accumulator. Written by the request / relocate / relight computes,
        // read back ONLY by the explicit diagnostics entry point — never inside
        // DDGIProbeUpdatePass::Execute, which is acceptance criterion 3 of #707.
        //
        // SSBO_DDGI_STATS is a handful of frame counters (live / relit /
        // captured probes) with the same read-back-on-demand contract, and is
        // where the "active probes is a small fraction of the dense grid" claim
        // is MEASURED rather than asserted.
        //
        // These were 79/80 while #707 was in review and moved to 82/83 when
        // #715's terrain virtual texture landed on master first and took 79-81.
        // Worth knowing if you hit the same thing: the collision merged CLEANLY
        // -- the two blocks are in different parts of this file, so git had no
        // textual conflict to report and produced a header with two constants
        // per slot. NOTHING caught it: SSBOSlotUniqueness is a hand-curated list
        // and neither family was on it, and the GLSL literals only disagree once
        // someone renumbers one side. Both families are on that list now. When
        // rebasing a branch that claims bindings, diff the slot NUMBERS against
        // master, not just the files git reports as conflicting.
        static constexpr u32 SSBO_DDGI_PROBE_AUX = 82; // DDGIProbeAux[totalProbes]: request frame, state, bounce coverage
        static constexpr u32 SSBO_DDGI_STATS = 83;     // DDGIStats: per-frame live/relit/captured counters

        // GPU prefix-sum / parallel scan (issue #713). Bound by
        // `GPUPrefixSum::ExclusiveScanInPlace` immediately before each of its
        // dispatches and never left bound — the scan is a leaf utility with no
        // engine-global published state, so these three numbers are private to
        // it and to `compute/PrefixSum_*.comp`.
        //
        // VALUES is scanned IN PLACE, which is safe precisely because each
        // invocation reads and writes exactly one index (its own): there is no
        // cross-invocation aliasing to order. That is what lets the recursion
        // over block totals run without a second ping-pong buffer per level.
        static constexpr u32 SSBO_PREFIX_SUM_VALUES = 54;     // u32[count]: scanned in place
        static constexpr u32 SSBO_PREFIX_SUM_BLOCK_SUMS = 55; // u32[workGroupCount]: this level's per-group totals
        static constexpr u32 SSBO_PREFIX_SUM_TOTAL = 56;      // u32[1]: grand total, written at the bottom level only

        // GPU terrain LOD quadtree (issue #714). The descent is a persistent
        // worklist: two ping-pong node lists whose roles swap every level, a
        // state block holding the counters AND both indirect-argument triples,
        // and three products (visible nodes, split map, LOD level map). All
        // eight are bound only while a Terrain*.comp kernel or a terrain draw is
        // in flight, but they get their own numbers rather than reusing an
        // unrelated system's the way the two-phase instance cull does — terrain
        // draws are ordinary scene geometry and share the frame with everything.
        //
        // NOT contiguous, deliberately. #714 and #713 were developed in parallel
        // and both took 54-56; #713 landed first (as #819) so it keeps them, and
        // only the three that collided moved — to 65-67 rather than 63-64, which
        // are spoken for (SSBO_BONE_PULL, and the reservation note below keeps
        // 63/64 clear of the sampler namespace). The five uncontested slots kept
        // their numbers so the diff shows the collision rather than hiding it in
        // a wholesale renumber.
        static constexpr u32 SSBO_TERRAIN_NODE_BOUNDS = 65;   // vec2[node]: world-space min/max Y, level-major
        static constexpr u32 SSBO_TERRAIN_NODE_LIST_IN = 66;  // uint[]: this level's pending packed node coords
        static constexpr u32 SSBO_TERRAIN_NODE_LIST_OUT = 67; // uint[]: children appended for the next level
        static constexpr u32 SSBO_TERRAIN_CULL_STATE = 58;    // TerrainGpuCullState: counters + dispatch args (also bound as GL_DISPATCH_INDIRECT_BUFFER)
        static constexpr u32 SSBO_TERRAIN_VISIBLE_NODES = 59; // uvec2[]: (packed coord, packed seam deltas), read by the terrain vertex stage via gl_InstanceIndex
        static constexpr u32 SSBO_TERRAIN_SPLIT_MAP = 60;     // uint[node]: 1 = this node split this frame
        static constexpr u32 SSBO_TERRAIN_LOD_MAP = 61;       // uint[(1<<depth)^2]: selected level per finest-node texel
        static constexpr u32 SSBO_TERRAIN_DRAW_ARGS = 62;     // DrawElementsIndirectCommand (also bound as GL_DRAW_INDIRECT_BUFFER, so it must be its own buffer at offset 0)

        // Terrain adaptive virtual texturing (issue #715). Three
        // buffers, and NO new UBO — deliberately: the UBO namespace has exactly
        // one free slot under the GL 4.6 minimum of 84 (UBO_BINDING_LIMIT is
        // 83), and a feature that wants a per-dispatch parameter block has no
        // business spending the last one. The bake and indirection kernels read
        // their parameters from a fixed HEADER at the front of their own SSBO
        // instead, and the shading-side parameters ride the existing
        // UBO_TERRAIN (10) block as three appended vec4s.
        //
        // 79-81, above the VSM range (68-78), so the two systems' ten-and-three
        // bindings cannot collide the way #713/#714 and #703/#818 did.
        static constexpr u32 SSBO_TERRAIN_VT_FEEDBACK = 79;    // uint[feedbackW*feedbackH], written by the terrain FRAGMENT stage
        static constexpr u32 SSBO_TERRAIN_VT_BAKE = 80;        // bake params header + VTBakeRequest[]
        static constexpr u32 SSBO_TERRAIN_VT_INDIRECTION = 81; // update-window header + VTIndirectionUpdate[]

        // The structured GPU readback-stats channel (issue #721). ONE 144-byte
        // block -- overflow-flag word, enable gate, frame index, 32 counter
        // slots -- that any GPU-driven pass may atomicAdd into, copied each frame
        // into a fenced device-to-host ring. GLSL twin:
        // include/GPUReadbackStats.glsl. C++ side: Renderer/Debug/GPUReadbackStats.h.
        //
        // 64, AND IT IS THE LAST NUMBER AVAILABLE. Read the SSBO_VSM_LOCAL_LIGHTS
        // note above first: this namespace is not merely tight, it is FULL.
        // Every value 0..83 (the GL 4.6 minimum guarantee, exclusive) is claimed
        // by *some* namespace; 57 and 63 are reserved engine-wide for the Vulkan
        // vertex-pull streams; and 64 is the only number never claimed as an
        // SSBO -- it is TEX_DDGI_VISIBILITY in the SAMPLER namespace.
        //
        // Reusing a number across namespaces is legal on GL (three disjoint
        // namespaces) and legal on Vulkan too, EXCEPT inside a single shader:
        // on Vulkan's single-set model a shader that reads sampler 64 and
        // storage 64 is a real collision -- that is precisely why
        // TEX_DDGI_VISIBILITY moved off 57 in issue #691 (ADR
        // item A2). Sampler 64 is read only through include/DDGICommon.glsl, so
        // the constraint has a checkable form: NO shader may include both
        // DDGICommon.glsl and GPUReadbackStats.glsl. That is asserted by
        // GPUReadbackStatsLayoutTest.NoStatsConsumerAlsoSamplesBinding64, and it
        // is the thing that will stop you if you try to publish counters from a
        // GI pass. The fix then is to renumber one side, not to delete the test.
        //
        // NOTHING IS LEFT AFTER THIS. The next feature that wants a buffer
        // binding has to either ride an existing block (the way #703 and #707
        // did) or renumber. Say so in your issue before you start.
        static constexpr u32 SSBO_GPU_STATS = 64;

        // The engine-wide Vulkan vertex-pull pair (ADR 0011 §5; issue #691,
        // ADR items A2/A3). On the Vulkan backend pipelines
        // carry no vertex-input state — a shader's OLO_VULKAN branch reads its
        // vertex data from these readonly SSBOs by gl_VertexIndex, and
        // VulkanRendererAPI::AssembleAndPushRootData maps each binding to the
        // draw's VAO streams (never to VulkanBindingState's published
        // buffers):
        //   57 = "OloVertexPull" — vertex stream 0 (the geometry stream every
        //        pulling shader reads; positions/normals/uvs).
        //   63 = "OloBonePull"   — vertex stream 1: whatever the VAO's second
        //        buffer holds. Bone influences are its NAMESAKE tenant
        //        (MeshSource's {uvec4 BoneIDs, vec4 Weights} — the 5 skinned
        //        shaders read 4 uint-bitcast + 4 float lanes, stride 8), but
        //        any second-stream VAO rides it: FoliageRenderer's 48-byte
        //        per-instance card records and ParticleBatchRenderer's 96-byte
        //        billboard instance records pull it by gl_InstanceIndex
        //        (issue #691); PBR_MultiLight.glsl's baked-lightmap UV2 stream
        //        (MeshSource's parallel vec2-per-vertex m_LightmapUVs buffer)
        //        rides it too (issue #866). The lightmap tenancy is safe
        //        because it is per-mesh MUTUALLY EXCLUSIVE with bones —
        //        MeshSource::Build only ever fills stream 1 with one of the
        //        two (!HasSkeleton() gates the lightmap buffer, HasSkeleton()
        //        gates the bone buffer) — and PBR_MultiLight.glsl (the only
        //        lightmap-sampling shader in v1) is never the shader bound for
        //        a skinned draw, so no single compiled shader ever declares
        //        binding 63 with two conflicting meanings. If a future mesh
        //        ever needs BOTH bone influences and a lightmap UV2 stream at
        //        once, this reuse breaks and a genuine third reserved number
        //        must be minted instead.
        // A VAO with fewer streams than a bound shader pulls resolves the
        // missing binding to the null (zero) address — deterministic zeros +
        // the draw path's warn-once, never a crash. Both numbers are RESERVED
        // engine-wide: nothing else may claim SSBO 57/63, and 63/64 stay out
        // of the sampler namespace too (the A2 renumber's lesson — see
        // TEX_DDGI_VISIBILITY above). GL never binds these numbers: the
        // attribute path serves the same streams through vertex-input state.
        static constexpr u32 SSBO_VERTEX_PULL = 57; // "OloVertexPull" — VAO stream 0 (Vulkan-only)
        static constexpr u32 SSBO_BONE_PULL = 63;   // "OloBonePull" — VAO stream 1, bone influences (Vulkan-only)

        // =============================================================================
        // TYPE ALIASES FOR CONVENIENCE
        // =============================================================================

        using CameraUBO = UBOStructures::CameraUBO;
        using MultiLightData = UBOStructures::MultiLightData;
        using MultiLightUBO = UBOStructures::MultiLightUBO;
        using MaterialUBO = UBOStructures::MaterialUBO;
        using PBRMaterialUBO = UBOStructures::PBRMaterialUBO;
        using ModelUBO = UBOStructures::ModelUBO;
        using AnimationUBO = UBOStructures::AnimationUBO;
        using IBLParametersUBO = UBOStructures::IBLParametersUBO;
        using IBLAdvancedParamsUBO = UBOStructures::IBLAdvancedParamsUBO;
        using ShadowUBO = UBOStructures::ShadowUBO;
        using TerrainUBO = UBOStructures::TerrainUBO;
        using TerrainCullUBO = UBOStructures::TerrainCullUBO;
        using LightProbeVolumeUBO = UBOStructures::LightProbeVolumeUBO;
        using LightmapUBO = UBOStructures::LightmapUBO;
        using BrushPreviewUBO = UBOStructures::BrushPreviewUBO;
        using FoliageUBO = UBOStructures::FoliageUBO;
        using DecalUBO = UBOStructures::DecalUBO;
        using WaterUBO = UBOStructures::WaterUBO;
        using GTAOUBO = UBOStructures::GTAOUBO;

        // =============================================================================
        // BINDING NAME VALIDATION
        // Keeps name-matching rules co-located with the binding constants so they
        // cannot drift out of sync.
        // =============================================================================

        static bool IsKnownUBOBinding(u32 binding, std::string_view name)
        {
            switch (binding)
            {
                case UBO_CAMERA:
                    return name.contains("Camera") || name.contains("camera");
                case UBO_MATERIAL:
                    return name.contains("Material") || name.contains("material") ||
                           name.contains("Particle") || name.contains("particle");
                case UBO_MODEL:
                    return name.contains("Model") || name.contains("model") ||
                           name.contains("Instance") || name.contains("instance");
                case UBO_ANIMATION:
                    return name.contains("Animation") || name.contains("animation") ||
                           name.contains("Bone") || name.contains("bone");
                case UBO_ANIMATION_PREV:
                    return name.contains("PrevBone") || name.contains("prevBone") ||
                           name.contains("PreviousBone") || name.contains("previousBone");
                case UBO_MULTI_LIGHTS:
                    return name.contains("MultiLight") || name.contains("multiLight");
                case UBO_SHADOW:
                    return name.contains("Shadow") || name.contains("shadow");
                case UBO_USER_0:
                    return true; // PostProcess
                case UBO_USER_1:
                    return true; // MotionBlur
                case UBO_SSAO:
                    return name.contains("SSAO") || name.contains("ssao");
                case UBO_TERRAIN:
                    return name.contains("Terrain") || name.contains("terrain");
                case UBO_BRUSH_PREVIEW:
                    return name.contains("Brush") || name.contains("brush");
                case UBO_FOLIAGE:
                    return name.contains("Foliage") || name.contains("foliage");
                case UBO_SNOW:
                    return name.contains("Snow") || name.contains("snow");
                case UBO_SSS:
                    return name.contains("SSS") || name.contains("sss");
                case UBO_WIND:
                    return name.contains("Wind") || name.contains("wind");
                case UBO_SNOW_ACCUMULATION:
                    return name.contains("SnowAccumulation") || name.contains("snowAccumulation");
                case UBO_FOG:
                    return name.contains("Fog") || name.contains("fog");
                case UBO_PRECIPITATION:
                    return name.contains("Precipitation") || name.contains("precipitation");
                case UBO_PRECIPITATION_SCREEN:
                    return name.contains("PrecipitationScreen") || name.contains("precipitationScreen");
                case UBO_FOG_VOLUMES:
                    return name.contains("FogVolumes") || name.contains("fogVolumes");
                case UBO_DECAL:
                    return name.contains("Decal") || name.contains("decal");
                case UBO_LIGHT_PROBES:
                    return name.contains("LightProbe") || name.contains("lightProbe");
                case UBO_LIGHTMAP:
                    return name.contains("Lightmap") || name.contains("lightmap");
                case UBO_WATER:
                    return name.contains("Water") || name.contains("water");
                case UBO_WATER_DISTURBANCE:
                    return name.contains("WaterDisturbance") || name.contains("waterDisturbance");
                case UBO_SHADER_GRAPH:
                    return name.contains("ShaderGraph") || name.contains("shaderGraph");
                case UBO_FORWARD_PLUS:
                    return name.contains("ForwardPlus") || name.contains("forwardPlus");
                case UBO_BOOT:
                    return name.contains("Boot") || name.contains("boot");
                case UBO_SELECTION_OUTLINE:
                    return name.contains("SelectionOutline") || name.contains("selectionOutline");
                case UBO_GTAO:
                    return name.contains("GTAO") || name.contains("gtao");
                case UBO_JUMP_FLOOD:
                    return name.contains("JumpFlood") || name.contains("jumpFlood");
                case UBO_DEFERRED_LIGHTING:
                    return name.contains("DeferredLighting") || name.contains("deferredLighting");
                case UBO_TAA:
                    return name.contains("TAA") || name.contains("taa") ||
                           name.contains("TemporalAA") || name.contains("temporalAA");
                case UBO_DRS:
                    return name.contains("DRS") || name.contains("DynamicResolution") ||
                           name.contains("dynamicResolution");
                case UBO_PREVIEW:
                    return name.contains("Preview") || name.contains("preview");
                case UBO_SH_COEFFICIENTS:
                    return name.contains("SHCoefficient") || name.contains("shCoefficient") ||
                           name.contains("SHCoeff") || name.contains("shCoeff");
                case UBO_PROCEDURAL_SKY:
                    return name.contains("ProceduralSky") || name.contains("proceduralSky") ||
                           name.contains("Preetham") || name.contains("preetham");
                case UBO_UNDERWATER:
                    return name.contains("Underwater") || name.contains("underwater");
                case UBO_SSR:
                    return name.contains("SSR") || name.contains("ssr");
                case UBO_STAR_NEST_SKY:
                    return name.contains("StarNest") || name.contains("starNest");
                case UBO_SSGI:
                    return name.contains("SSGI") || name.contains("ssgi");
                case UBO_CONTACT_SHADOW:
                    return name.contains("ContactShadow") || name.contains("contactShadow");
                case UBO_MOTION_BLUR_PARAMS:
                    return name.contains("MotionBlur") || name.contains("motionBlur");
                case UBO_PLANAR_REFLECTION:
                    return name.contains("PlanarReflection") || name.contains("planarReflection");
                case UBO_UPSCALER:
                    return name.contains("CAS") || name.contains("cas") ||
                           name.contains("RCAS") || name.contains("rcas") ||
                           name.contains("Upscaler") || name.contains("upscaler");
                case UBO_EASU:
                    return name.contains("EASU") || name.contains("easu");
                case UBO_FROXEL_FOG:
                    return name.contains("FroxelFog") || name.contains("froxelFog");
                case UBO_FLUID:
                case UBO_FLUID_RENDER:
                    return name.contains("Fluid") || name.contains("fluid");
                case UBO_VIRTUAL_DRAW:
                    return name.contains("VirtualDraw");
                case UBO_VIRTUAL_DEBUG:
                    return name.contains("VirtualDebug");
                case UBO_DDGI:
                    return name.contains("DDGI");
                case UBO_ATMOSPHERE_SKY:
                    return name.contains("AtmosphereSky") || name.contains("atmosphereSky");
                case UBO_CLOUDSCAPE:
                    return name.contains("Cloudscape") || name.contains("cloudscape");
                case UBO_ATMOSPHERE_SHADING:
                    return name.contains("AtmosphereShading") || name.contains("atmosphereShading");
                case UBO_IMPOSTOR_BAKE:
                    return name.contains("ImpostorBake") || name.contains("impostorBake");
                case UBO_DEBUG_DRAW:
                    return name.contains("DebugDraw") || name.contains("debugDraw");
                case UBO_REFLECTION_PROBES:
                    return name.contains("ReflectionProbe") || name.contains("reflectionProbe");
                case UBO_AUTO_EXPOSURE:
                    return name.contains("AutoExposure") || name.contains("autoExposure");
                case UBO_HZB:
                    return name.contains("HZB") || name.contains("hzb");
                case UBO_GTAO_DENOISE:
                    return name.contains("GTAODenoise") || name.contains("gtaoDenoise");
                // Issue #691 compute bare-uniform sweep.
                case UBO_PARTICLE_SIM:
                    return name.contains("ParticleSim") || name.contains("particleSim");
                case UBO_WIND_GENERATE:
                    return name.contains("WindGenerate") || name.contains("windGenerate");
                case UBO_SNOW_COMPUTE:
                    return name.contains("SnowCompute") || name.contains("snowCompute");
                case UBO_TERRAIN_EROSION:
                    return name.contains("TerrainErosion") || name.contains("terrainErosion");
                case UBO_LIGHT_CULLING:
                    return name.contains("LightCulling") || name.contains("lightCulling");
                case UBO_VIRTUAL_CLUSTER_CULL:
                    return name.contains("VirtualClusterCull") || name.contains("virtualClusterCull");
                case UBO_VIRTUAL_RASTER:
                    return name.contains("VirtualRaster") || name.contains("virtualRaster");
                case UBO_INSTANCE_CULL:
                    return name.contains("InstanceCull") || name.contains("instanceCull");
                // Issue #691 — the sweep's completion.
                case UBO_OCEAN_FFT:
                    return name.contains("OceanFFT") || name.contains("oceanFFT");
                case UBO_CLOUD_NOISE_GEN:
                    return name.contains("CloudNoiseGen") || name.contains("cloudNoiseGen");
                case UBO_CLOUD_SHADOW_GEN:
                    return name.contains("CloudShadowGen") || name.contains("cloudShadowGen");
                case UBO_PRECIPITATION_FEED:
                    return name.contains("PrecipitationFeed") || name.contains("precipitationFeed");
                case UBO_REFLECTION_PROBE_CULL:
                    return name.contains("ReflectionProbeCull") || name.contains("reflectionProbeCull");
                case UBO_COLORBLIND:
                    return name.contains("ColorBlind") || name.contains("colorBlind");
                case UBO_VIRTUAL_SHADOW:
                case UBO_VIRTUAL_SHADOW_DRAW:
                    return name.contains("VirtualShadow") || name.contains("virtualShadow");
                // Issue #713 — GPU prefix-sum / parallel scan.
                case UBO_PREFIX_SUM:
                    return name.contains("PrefixSum") || name.contains("prefixSum");
                // Issue #714 — GPU terrain LOD quadtree descent.
                case UBO_TERRAIN_CULL:
                    return name.contains("TerrainCull") || name.contains("terrainCull");
                default:
                    return false;
            }
        }

        static bool IsKnownTextureBinding(u32 binding, std::string_view name)
        {
            switch (binding)
            {
                case TEX_DIFFUSE:
                    return name.contains("Diffuse") || name.contains("diffuse") ||
                           name.contains("Albedo") || name.contains("albedo") ||
                           name == "u_Texture" || name == "u_Textures" ||
                           // Slot 0 is also used as generic input for many shaders
                           name.contains("Scene") || name.contains("FontAtlas") ||
                           name.contains("Equirectangular") || name.contains("SSAO") ||
                           // Post-process / fullscreen passes reuse slot 0 with
                           // pass-local input names.
                           name == "u_Current" || name == "u_EntityID" ||
                           name == "u_CurveTexture" || name.contains("Overdraw") ||
                           // Compute dispatches never coexist with a bound material
                           // (the engine rebinds slot 0 per-dispatch, same as
                           // TEX_USER_0-2's documented pass-local reuse) — issue #627.
                           name == "u_HDRColor" || name == "u_ScatterVolume" ||
                           name == "u_ShadowMapCSM" || name == "u_HZB" ||
                           name == "u_Butterfly" || name == "u_H0" ||
                           // DDGI fullscreen-pass pass-local reuse (issue #632).
                           name == "u_Radiance" || name == "u_HitGeo" ||
                           // Cloudscape temporal resolve (issue #633): this
                           // frame's half-res raymarch at the pass-local slot 0
                           // (fullscreen pass, no material bound).
                           name == "u_CloudCurrent" ||
                           // Virtual-geometry debug overlay (issue #629): a fullscreen pass
                           // whose only input is the cluster/LOD/overdraw image, composited
                           // over the lit frame at the end of DeferredLightingPass. Same
                           // pass-local slot-0 reuse as the entries above — no material is
                           // bound during a fullscreen draw.
                           name == "u_VirtualDebugColor" ||
                           // SSR / SSGI temporal resolve (issue #902): the
                           // resolve draw's input is this frame's raw stochastic
                           // signal. Same pass-local slot-0 reuse as the
                           // fullscreen entries above — no material is bound.
                           name == "u_StochasticSignal";
                case TEX_SPECULAR:
                    // Slot 1 is reused across shader contexts: Metallic/Roughness in PBR,
                    // Depth textures in particle effects, Bloom textures in post-processing,
                    // history / overlay buffers in fullscreen passes.
                    return name.contains("Specular") || name.contains("specular") ||
                           name.contains("Metallic") || name.contains("metallic") ||
                           name.contains("Depth") || name.contains("Bloom") ||
                           name == "u_History" || name == "u_FogTexture" ||
                           name == "u_BandTexture" || name == "u_JFAResult" ||
                           name == "u_EntityID" ||
                           // Compute dispatch pass-local reuse (issue #627).
                           name == "u_ShadowAtlas" ||
                           // DDGI fullscreen-pass pass-local reuse (issue #632).
                           name == "u_HitGeo" || name == "u_CaptureGeo" || name == "u_PrevVisibility" ||
                           // DDGI capture VERTEX stage (issue #707). Unlike every other
                           // entry here this one is not a fullscreen pass: DDGI_Capture
                           // rasterizes real geometry, so a material could in principle
                           // occupy slot 1. It cannot here — the capture pass binds its
                           // own units explicitly (0 = caster albedo, 1 = probe data) and
                           // never goes through the material system. The vertex stage
                           // needs it because #707 moved probe-position derivation onto
                           // the GPU: capturing from the lattice point while relight
                           // reconstructs from the RELOCATED one would offset every
                           // cached hit. u_ProbeData is also allowlisted at TEX_HEIGHT
                           // for the #632 fullscreen passes; both are pass-local.
                           name == "u_ProbeData" ||
                           // Cloudscape passes (issue #633): the temporal
                           // resolve samples last frame's history, the
                           // composite samples the resolved half-res clouds —
                           // both pass-local slot-1 reuse in fullscreen draws.
                           name == "u_CloudHistory" || name == "u_CloudResolved" ||
                           // SSR / SSGI temporal resolve (issue #902): the
                           // composite draw samples the resolved signal, the
                           // resolve draw samples last frame's history
                           // (u_History, already listed above). Both are
                           // pass-local slot-1 reuse in fullscreen draws.
                           name == "u_ResolvedSignal";
                case TEX_NORMAL:
                    return name.contains("Normal") || name.contains("normal") ||
                           // Slot 2 is reused as the velocity input slot for TAA / motion-blur passes.
                           name == "u_Velocity" ||
                           // Compute dispatch pass-local reuse (issue #627).
                           name == "u_HistoryVolume" ||
                           // DDGI fullscreen-pass pass-local reuse (issue #632).
                           name == "u_PrevIrradiance";
                case TEX_HEIGHT:
                    return name.contains("Height") || name.contains("height") ||
                           name.contains("Displacement") || name.contains("displacement") ||
                           // Slot 3 is reused as the fog-history input slot for the fog pass.
                           name == "u_FogHistory" ||
                           // Compute dispatch pass-local reuse (issue #627).
                           name == "u_HZBDepth" ||
                           // DDGI fullscreen-pass pass-local reuse (issue #632).
                           name == "u_ProbeData" || name == "u_CurrVisibility" ||
                           // OpenVDB-imported density volume, compute-local reuse
                           // in FroxelFogScatter.comp (issue #724).
                           name == "u_DensityVolume" ||
                           // VRCS classification (issue #683): VRCSClassify.comp
                           // reads scene depth here, mirroring GTAO.comp's
                           // pass-local 3/4/5 so the two compute dispatches keep
                           // the same slot meanings.
                           name == "u_SceneDepth";
                case TEX_AMBIENT:
                    return name.contains("AO") || name.contains("Ambient") ||
                           name.contains("ambient") || name.contains("Occlusion") ||
                           name.contains("occlusion") ||
                           // Compute dispatch pass-local reuse (issue #627).
                           name == "u_ViewNormals" || name == "u_InputDepth" ||
                           // DDGI fullscreen-pass pass-local reuse (issue #632).
                           name == "u_ProbeData";
                case TEX_EMISSIVE:
                    return name.contains("Emissive") || name.contains("emissive") ||
                           name.contains("Emission") || name.contains("emission") ||
                           // Compute dispatch pass-local reuse (issue #627).
                           name == "u_HilbertLUT" ||
                           // VRCS classification (issue #683): the previous
                           // frame's resolved colour, for the luminance-variance
                           // term. Pass-local, same as u_HilbertLUT above.
                           name == "u_PrevSceneColor";
                case TEX_ROUGHNESS:
                    return name.contains("Roughness") || name.contains("roughness") ||
                           // VRCS (issue #683): GTAO.comp reads the per-tile
                           // shading-rate image here, one slot past its existing
                           // pass-local 3/4/5. A compute dispatch never coexists
                           // with a bound material, which is what makes the
                           // low-slot reuse above safe and makes this safe too.
                           name == "u_ShadingRate";
                case TEX_METALLIC:
                    return name.contains("Metallic") || name.contains("metallic");
                case TEX_SHADOW:
                    return name.contains("Shadow") || name.contains("shadow");
                case TEX_ENVIRONMENT:
                    return name.contains("Environment") || name.contains("environment") ||
                           name.contains("Skybox") || name.contains("skybox") ||
                           name.contains("Cubemap");
                case TEX_REFLECTION_PROBE_RADIANCE:
                case TEX_REFLECTION_PROBE_DISTANCE:
                    return name.contains("Probe") || name.contains("probe");
                case TEX_LIGHTMAP:
                    return name.contains("Lightmap") || name.contains("lightmap");
                case TEX_WIND_FIELD:
                    return name.contains("Wind") || name.contains("wind");
                case TEX_SNOW_DEPTH:
                    return name.contains("SnowDepth") || name.contains("snowDepth");
                case TEX_PRECIPITATION_NOISE:
                    return name.contains("Precipitation") || name.contains("precipitation") ||
                           name.contains("StreakNoise") || name.contains("streakNoise");
                case TEX_WATER_NORMAL_0:
                case TEX_WATER_NORMAL_1:
                    return name.contains("WaterNormal") || name.contains("waterNormal") ||
                           (name.contains("Water") && name.contains("Normal")) ||
                           // Production water shaders use unprefixed names (Water.glsl,
                           // Water_OIT.glsl bind u_NormalMap0 / u_NormalMap1 to these slots).
                           name == "u_NormalMap0" || name == "u_NormalMap1";
                case TEX_WATER_NOISE:
                    return name.contains("WaterNoise") || name.contains("waterNoise") ||
                           (name.contains("Water") && name.contains("Noise")) ||
                           // Production water shaders bind u_NoiseMap to this slot.
                           name == "u_NoiseMap";
                case TEX_WATER_DEPTH:
                    return name.contains("WaterDepth") || name.contains("waterDepth") ||
                           (name.contains("Scene") && name.contains("Depth"));
                case TEX_WATER_REFRACTION:
                    return name.contains("Refraction") || name.contains("refraction");
                case TEX_WATER_FOAM:
                    return name.contains("Foam") || name.contains("foam");
                case TEX_WATER_DISTURBANCE:
                    return name.contains("Disturbance") || name.contains("disturbance");
                case TEX_WATER_SSR:
                    return name.contains("SSR") || name.contains("ssr") ||
                           (name.contains("Screen") && name.contains("Reflection"));
                case TEX_WATER_PLANAR_REFLECTION:
                    return name.contains("PlanarReflection") || name.contains("planarReflection");
                case TEX_WATER_FFT_DISPLACEMENT:
                case TEX_WATER_FFT_DERIVATIVES:
                    // FFT ocean cascade textures: Water.glsl binds
                    // u_FFTDisplacement / u_FFTDerivatives to these slots.
                    return name.contains("FFT") || name.contains("fft") ||
                           name.contains("Displacement") || name.contains("displacement") ||
                           name.contains("Derivatives") || name.contains("derivatives");
                case TEX_GBUFFER_ALBEDO:
                case TEX_GBUFFER_NORMAL:
                case TEX_GBUFFER_EMISSIVE:
                case TEX_GBUFFER_VELOCITY:
                case TEX_GBUFFER_DEPTH:
                case TEX_GBUFFER_BAKEDGI:
                    // Engine convention is `u_G<Attr>` (u_GAlbedo, u_GNormal,
                    // u_GEmissive, u_GVelocity, u_GDepth); some debug shaders also
                    // spell it `GBuffer*`.
                    return name.contains("GBuffer") || name.contains("gBuffer") ||
                           name.contains("gbuffer") ||
                           name == "u_GAlbedo" || name == "u_GNormal" ||
                           name == "u_GEmissive" || name == "u_GVelocity" ||
                           name == "u_GDepth";
                case TEX_OIT_ACCUM:
                case TEX_OIT_REVEALAGE:
                    return name.contains("OIT") || name.contains("oit") ||
                           name.contains("Accum") || name.contains("accum") ||
                           name.contains("Revealage") || name.contains("revealage");
                case TEX_UNDERWATER_WATER_DEPTH:
                    // Dedicated one-off slot (32) for the underwater-fog water-surface
                    // depth; validate its sampler name instead of letting it pass the
                    // generic 10–42 range unchecked. Production: u_WaterSurfaceDepth.
                    return name == "u_WaterSurfaceDepth" ||
                           (name.contains("Water") && name.contains("Surface") && name.contains("Depth"));
                case TEX_FROXEL_FOG:
                    // Integrated froxel fog volume (issue #435): u_FroxelFogVolume.
                    return name.contains("FroxelFog") || name.contains("froxelFog");
                case TEX_FLUID_DEPTH:
                case TEX_FLUID_THICKNESS:
                    // Screen-space fluid intermediates (issue #630):
                    // u_FluidDepth / u_FluidThickness.
                    return name.contains("Fluid") || name.contains("fluid");
                case TEX_DDGI_IRRADIANCE:
                case TEX_DDGI_VISIBILITY:
                case TEX_DDGI_PROBE_DATA:
                    // Realtime DDGI probe atlases (issue #632):
                    // u_DDGIIrradianceAtlas / u_DDGIVisibilityAtlas / u_DDGIProbeData.
                    return name.contains("DDGI");
                // Volumetric cloudscape inputs (issue #633). Exact names, one
                // per slot — a shared contains("Cloud") would silently accept a
                // shader whose base/detail/weather/shadow bindings are swapped.
                case TEX_CLOUD_BASE_NOISE:
                    return name == "u_CloudBaseNoise";
                case TEX_CLOUD_DETAIL_NOISE:
                    return name == "u_CloudDetailNoise";
                case TEX_CLOUD_WEATHER_MAP:
                    return name == "u_CloudWeatherMap";
                case TEX_CLOUD_SHADOW:
                    return name == "u_CloudShadowMap";
                case TEX_VSM_PHYSICAL:
                    // Virtual Shadow Map physical page pool (issue #702).
                    return name == "u_VSMPhysicalPool";
                case TEX_VOLUMETRIC_SHADOW:
                    // Shared volumetric shadow volume (issue #723).
                    return name == "u_VolumetricShadowVolume";
                case TEX_TERRAIN_VT_INDIRECTION:
                    // Terrain virtual texture (issue #715). Both declared once, in
                    // include/TerrainVirtualTexture.glsl.
                    return name == "u_TerrainVTIndirection";
                case TEX_TERRAIN_VT_CACHE:
                    return name == "u_TerrainVTCache";
                default:
                    // Accept explicitly defined engine texture slots (TEX_USER_0 through TEX_WATER_SSR, i.e. 10–42)
                    // and shader graph user texture slots (TEX_SHADER_GRAPH_0+)
                    return (binding >= TEX_USER_0 && binding <= TEX_WATER_SSR) ||
                           (binding >= TEX_SHADER_GRAPH_0 && binding < 80);
            }
        }

        // =============================================================================
        // GLSL LAYOUT STRINGS FOR CODE GENERATION
        // =============================================================================

        // Documentation helper: the GLSL block text below mirrors the runtime
        // `UBOStructures::CameraUBO` struct verbatim (including the trailing
        // `u_PrevViewProjection`). Shaders that don't need the prev-frame
        // matrix can still declare a shorter block thanks to std140 trailing-
        // byte tolerance; new shaders should prefer this full layout so
        // motion-vector aware pipelines get the correct member offsets.
        static const char* GetCameraUBOLayout()
        {
            return R"(
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};)";
        }

        static std::string GetMultiLightUBOLayout()
        {
            return std::string(R"(
struct LightData {
    vec4 position;         // Position in world space (w = light type)
    vec4 direction;        // Direction for directional/spot lights
    vec4 color;            // Light color and intensity (w = intensity)
    vec4 attenuationParams; // (constant, linear, quadratic, range)
    vec4 spotParams;       // (inner_cutoff, outer_cutoff, falloff, enabled)
};

layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int u_MaxLights;
    int u_ShadowCasterCount;
    int u_DirectionalLightCount;
    LightData u_Lights[)") +
                   std::to_string(UBOStructures::MultiLightUBO::MAX_LIGHTS) + R"(];
};)";
        }

        static const char* GetMaterialUBOLayout()
        {
            return R"(
layout(std140, binding = 2) uniform MaterialProperties {
    vec4 u_MaterialAmbient;
    vec4 u_MaterialDiffuse;
    vec4 u_MaterialSpecular;
    vec4 u_MaterialEmissive;
    int u_UseTextureMaps;
    int u_AlphaMode;
    int u_DoubleSided;
    int _padding;
};)";
        }

        static const char* GetPBRMaterialUBOLayout()
        {
            return R"(
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;
    vec4 u_EmissiveFactor;
    float u_MetallicFactor;
    float u_RoughnessFactor;
    float u_NormalScale;
    float u_OcclusionStrength;
    int u_UseAlbedoMap;
    int u_UseNormalMap;
    int u_UseMetallicRoughnessMap;
    int u_UseAOMap;
    int u_UseEmissiveMap;
    int u_EnableIBL;
    int u_ApplyGammaCorrection;
    float u_AlphaCutoff;
    int u_EnableLightProbes;
    float u_IBLIntensity;
    int u_AlphaMode;
    int u__pbrPad2;
};)";
        }

        // Documentation helper: the GLSL block text below mirrors the runtime
        // `UBOStructures::ModelUBO` struct verbatim (including the trailing
        // `u_PrevModel`). Legacy shaders that don't sample the prev-frame
        // world transform can still declare a shorter block thanks to std140
        // trailing-byte tolerance; new shaders should prefer this full layout
        // so per-object motion-vector paths get the correct member offsets.
        static const char* GetModelUBOLayout()
        {
            return R"(
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};)";
        }

        static std::string GetAnimationUBOLayout()
        {
            // Compile-time validation that GLSL array size matches C++ constant
            constexpr u32 glsl_max_bones = UBOStructures::AnimationConstants::MAX_BONES;
            static_assert(glsl_max_bones == UBOStructures::AnimationUBO::MAX_BONES,
                          "GLSL MAX_BONES must match C++ AnimationUBO::MAX_BONES");

            return std::string(R"(
layout(std140, binding = 4) uniform AnimationMatrices {
    mat4 u_BoneMatrices[)") +
                   std::to_string(glsl_max_bones) + R"(];
};)";
        }

        // Compile-time validation for shader constant consistency
        static_assert(UBOStructures::AnimationUBO::MAX_BONES == UBOStructures::AnimationConstants::MAX_BONES,
                      "Animation shader constants must be consistent across UBO structures");

        static const char* GetStandardTextureBindings()
        {
            return R"(
layout(binding = 0) uniform sampler2D u_DiffuseMap;
layout(binding = 1) uniform sampler2D u_SpecularMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 3) uniform sampler2D u_HeightMap;
layout(binding = 4) uniform sampler2D u_AmbientMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
layout(binding = 6) uniform sampler2D u_RoughnessMap;
layout(binding = 7) uniform sampler2D u_MetallicMap;)";
        }

        static const char* GetPBRTextureBindings()
        {
            return R"(
layout(binding = 0) uniform sampler2D u_AlbedoMap;
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap;
layout(binding = 2) uniform sampler2D u_NormalMap;
layout(binding = 4) uniform sampler2D u_AOMap;
layout(binding = 5) uniform sampler2D u_EmissiveMap;
layout(binding = 9) uniform samplerCube u_EnvironmentMap;
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D u_BRDFLutMap;)";
        }

        static std::string GetShadowUBOLayout()
        {
            // The atlas array sizes come from the C++ struct constant so this
            // GLSL reference block can never silently drift from ShadowUBO.
            static const std::string s_Layout =
                std::string(R"(
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
    mat4 u_AtlasEntryMatrices[)") +
                std::to_string(UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES) +
                R"(];    // light VP per shadow-atlas entry (spot = 1 entry, point = 6 face entries)
    vec4 u_AtlasEntryScaleOffset[)" +
                std::to_string(UBOStructures::ShadowUBO::MAX_SHADOW_ATLAS_ENTRIES) +
                R"(]; // xy = UV scale, zw = UV offset of the entry's atlas tile
    int u_DirectionalShadowEnabled;
    int u_AtlasEntryCount;
    int u_ShadowMapResolution;
    int u_AtlasResolution;
    int u_CascadeDebugEnabled;
    int u_SoftShadowMode;  // 0 = legacy hardware PCF, 1 = PCSS (contact-hardening)
    int _shadowPad1;
    int _shadowPad2;
};)";
            return s_Layout;
        }

        static const char* GetDecalUBOLayout()
        {
            return R"(
layout(std140, binding = 21) uniform DecalData {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection;
    vec4 u_DecalColor;
    vec4 u_DecalParams; // x = fadeDistance, y = normalAngleThreshold, z/w = unused
};)";
        }

        static const char* GetWaterUBOLayout()
        {
            return R"(
layout(std140, binding = 23) uniform WaterParams {
    vec4 u_WaveParams;              // x = Time, y = WaveSpeed, z = WaveAmplitude, w = WaveFrequency
    vec4 u_WaveDir0;                // xy = direction0, z = steepness0, w = wavelength0
    vec4 u_WaveDir1;                // xy = direction1, z = steepness1, w = wavelength1
    vec4 u_WaterColor;              // rgb = shallow color, a = Transparency
    vec4 u_WaterDeepColor;          // rgb = deep color,    a = Reflectivity
    vec4 u_VisualParams;            // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
    vec4 u_NormalMapScroll;         // xy = scroll0 offset, zw = scroll1 offset
    vec4 u_NormalMapSpeed;          // x = speed0, y = speed1, z = PrevTime (for Gerstner reprojection), w = renderFromBelow
    vec4 u_LightDirection;          // xyz = directional light dir (normalized), w = unused
    vec4 u_ScreenParams;            // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;   // x = depthSofteningDist, y = refractionDistortion, z = refractionHeightFactor, w = unused
    vec4 u_RefractionColor;         // rgb = underwater tint, w = unused
    vec4 u_FoamParams;              // x = foamHeightStart, y = foamFadeDistance, z = foamTiling, w = foamBrightness
    vec4 u_FoamParams2;             // x = foamAngleExponent, y = shorelineFoamPower, z = sssIntensity, w = vertexSpacing (#943)
    vec4 u_SSSColor;                // rgb = subsurface scattering color, w = foamCoverage (#943)
    vec4 u_SSRParams;               // x = maxSteps, y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;              // x = tessellationFactor (0 = disabled), y = minDist, z = maxDist, w = frustumCullEnable (1=on, 0=off)
    vec4 u_FFTParams;               // x = useFFT (0/1), y = 1/patchSize, z = heightScale, w = horizontalScale
    vec4 u_WakeFieldParams;         // xy = field window centre (world XZ), z = 1/fieldExtent, w = intensity (<=0 disables) (#967)
    vec4 u_WakeFieldParams2;        // x = wake fade start (m), y = wake fade end (m), z = edge-fade start, w = unused (#967)
};)";
        }

        static const char* GetForwardPlusUBOLayout()
        {
            return R"(
layout(std140, binding = 25) uniform ForwardPlusParams {
    uvec4 fplus_Params;       // x = ClusterCountX, y = ClusterCountY, z = Enabled (0/1), w = ClusterCountZ
    vec4  fplus_TileScale;    // xy = clusterCount / screenSize, zw = unused
    vec4  fplus_DepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar
};)";
        }

        // Per-instance data SSBO indexed by gl_InstanceIndex. Layout mirrors
        // OloEngine::InstanceData (Renderer/Instancing/InstanceData.h, 240 B
        // std430). Shaders migrating from the legacy ModelUBO (binding = 3)
        // replace `u_Model` with `instances[gl_InstanceIndex].Transform`,
        // `u_Normal` with `instances[gl_InstanceIndex].Normal`, etc. Non-
        // instanced draws bind a single-element instance buffer so the same
        // shader body works in both cases.
        static const char* GetInstanceSSBOLayout()
        {
            return R"(
struct InstanceData {
    mat4 Transform;
    mat4 Normal;
    mat4 PrevTransform;
    vec4 Color;
    int  EntityID;
    float Custom;
    int  _pad0;
    int  _pad1;
    vec4 LightmapScaleOffset;
};

layout(std430, binding = 15) readonly buffer InstanceBuffer {
    InstanceData instances[];
};)";
        }
    };
} // namespace OloEngine

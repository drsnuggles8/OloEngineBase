#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"
#include "OloEngine/Terrain/Foliage/FoliageLodTransition.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"
#include "OloEngine/Renderer/Model.h"

#include <glm/glm.hpp>
#include <array>
#include <string>
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    class VertexArray;
    class VertexBuffer;
    class IndexBuffer;
    class Shader;
    class TerrainData;
    class TerrainMaterial;
    class Frustum;

    // Lightweight POD struct exposing per-layer data needed for command submission.
    // Avoids leaking internal LayerRenderData internals (Ref<VertexArray> etc.).
    // Uses u32 for GL resource IDs to avoid pulling in RenderCommand.h.
    struct FoliageLayerDrawInfo
    {
        // Physical layer slot this draw came from. Carried so identity
        // survives command submission (issue #1230): a consumer holding a
        // draw can reach the layer's canonical records and spatial groups via
        // FoliageRenderer::GetInstanceRegistry(). Not the index of this entry
        // in the returned vector — inactive layers are skipped, and since
        // #1233 ONE layer can contribute SEVERAL entries (one per authored-mesh
        // submesh plus the card), so this is not a one-to-one index either.
        u32 LayerIndex = 0;
        RHI::ResourceHandle VertexArrayID{};
        // Index range within VertexArrayID's index buffer. The card is the
        // whole buffer; an authored mesh's submeshes are consecutive ranges of
        // one shared buffer, each with its own material (issue #1233).
        u32 BaseIndex = 0;
        u32 IndexCount = 0;
        // The instance count the CPU knows about: every instance the generator
        // placed for this layer. With GPU culling active this is NOT what the
        // draw draws -- the indirect command's instanceCount is, and it lives on
        // the GPU. Kept because it is the "generated" figure the profiler and
        // the census report, and because it is exactly what the draw falls back
        // to when culling is unavailable.
        u32 InstanceCount = 0;
        // GPU cull result (issue #1235). When valid, VertexArrayID above is the
        // vertex array over the COMPACTED instance stream and the dispatcher
        // issues DrawBoundElementsIndirect from this buffer at this offset
        // instead of an instanced draw of InstanceCount. Null = the uncompacted
        // path, which is the correct frame either way.
        RHI::ResourceHandle IndirectBufferID{};
        u32 IndirectOffsetBytes = 0;
        RHI::ResourceHandle AlbedoTextureID{};
        // This entry draws the layer's authored plant mesh rather than the flat
        // card. The vertex stage needs it: a card is scaled anisotropically
        // (x/z by scale, y by height * scale) and a mesh UNIFORMLY by
        // height * scale, matching the impostor so the silhouette does not jump
        // at the hand-over.
        bool IsAuthoredMesh = false;
        // The mesh-to-card hand-over band, IDENTICAL on both of a layer's
        // entries — that is what lets the two draws partition the pixels
        // exactly (see the FoliageMeshLod.glsl include) instead of each running
        // its own fade and leaving a stretch where a pine and its card are both
        // opaque. Zero end means the layer has no authored mesh and the card
        // covers everything, which is the pre-#1233 behaviour exactly.
        f32 MeshHandoverStartDistance = 0.0f;
        f32 MeshHandoverEndDistance = 0.0f;
        f32 ViewDistance = 100.0f;
        f32 FadeStartDistance = 80.0f;
        f32 WindStrength = 0.3f;
        f32 WindSpeed = 1.0f;
        glm::vec4 WindWeights{ 0.0f };
        // This layer's answer to the scene's interaction field (issue #1238).
        // The influences themselves are global and read from
        // FoliageInteractionField at UBO-fill time — only the per-species
        // response rides the draw.
        f32 InteractionResponse = 1.0f;
        glm::vec3 BaseColor{ 1.0f };
        f32 AlphaCutoff = 0.5f;
        BoundingBox Bounds; // Precomputed AABB encompassing all instances in this layer

        // Octahedral impostor LOD (issue #433). UseImpostor + valid atlas IDs
        // route this layer through the impostor card shader instead of the flat
        // billboard; zero/false leaves the existing billboard path untouched.
        // ── The layer's LEAF MATERIAL (issue #1234) ──────────────────────
        // Carried on EVERY draw a layer emits — the authored mesh, the flat
        // card and the impostor card — so a plant cannot change what it is
        // made of as it crosses a LOD hand-over. A null map handle means the
        // layer authored none (or the file would not open), and the shader
        // then uses the authored constant rather than sampling a typed null.
        //
        // LeafTransmissionStrength == 0 is the off switch and the default:
        // everything downstream then behaves exactly as it did before #1234.
        RHI::ResourceHandle LeafNormalTextureID{};
        RHI::ResourceHandle LeafRoughnessTextureID{};
        RHI::ResourceHandle LeafThicknessTextureID{};
        f32 LeafRoughness = 0.8f;
        f32 LeafNormalStrength = 1.0f;
        f32 LeafThickness = 0.0f;
        f32 LeafTransmissionStrength = 0.0f;
        glm::vec3 LeafTransmissionColor{ 0.42f, 0.62f, 0.18f };
        f32 LeafTransmissionDistortion = 0.35f;
        f32 LeafTransmissionPower = 4.0f;
        f32 LeafTransmissionWrap = 0.5f;
        f32 LeafTransmissionAmbient = 0.35f;

        bool UseImpostor = false;
        RHI::ResourceHandle ImpostorAlbedoAtlasID{};
        RHI::ResourceHandle ImpostorNormalDepthAtlasID{};
        u32 ImpostorFramesPerAxis = 8;
        bool ImpostorHemi = true;
        f32 ImpostorStartDistance = 40.0f;
        f32 ImpostorTransitionBand = 15.0f;
        f32 ImpostorRadius = 1.0f;

        // LOD transitions + coverage-preserving density (issue #1237), already
        // packed into the two lanes FoliageUBO::LodTransition0/1 carries and
        // IDENTICAL on every draw a layer emits — the same reason the leaf
        // material above is. A plant that thinned on the card rung and not on
        // the mesh rung would change size as it crossed the hand-over, which is
        // exactly the pop the feature removes.
        glm::vec4 LodTransition0{ 0.0f, 30.0f, 80.0f, 0.25f };
        glm::vec4 LodTransition1{ 0.15f, 2.0f, 0.0f, 0.0f };
    };

    // Manages foliage instance generation, culling, and instanced rendering.
    // Generates instances on the CPU from terrain data + foliage layer config,
    // uploads to a per-layer instance VBO, and draws with DrawIndexedInstanced.
    struct FoliageLayerDrawPart
    {
        u32 BaseIndex = 0;
        u32 IndexCount = 0;
        // The submesh's own albedo. Null falls through to the layer's
        // AlbedoTexture, which is what a mesh with no imported texture gets.
        Ref<Texture2D> Albedo;
    };

    // An external texture Ref and scalar index range; no self-address escapes.
    template<>
    struct TIsTriviallyRelocatable<FoliageLayerDrawPart>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageLayerDrawPart::BaseIndex)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDrawPart::IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDrawPart::Albedo)>::Value;
    };

    struct FoliageLayerRenderData
    {
        Ref<VertexArray> VAO;
        Ref<VertexBuffer> QuadVBO;     // Geometry (unit quad)
        Ref<VertexBuffer> InstanceVBO; // Per-instance data
        Ref<IndexBuffer> IBO;

        // Authored plant mesh (issue #1233), drawn up close. A SEPARATE
        // vertex array over a PRIVATE copy of the source geometry rather
        // than the MeshSource's own: the instance stream has to be bound
        // into the vertex array, and doing that to a shared mesh asset
        // would leak this layer's instancing into every other user of it.
        Ref<VertexArray> MeshVAO;
        Ref<VertexBuffer> MeshVBO;
        Ref<IndexBuffer> MeshIBO;
        TArray<FoliageLayerDrawPart> MeshParts;
        TArray<u32> MeshRayTracingIndices;
        // Keeps the imported materials' textures alive for as long as the
        // parts reference them.
        Ref<Model> MeshModel;
        FString MeshGeometryPath;   // What MeshVBO/MeshIBO were built from
        bool MeshRequested = false; // UseAuthoredMesh && !MeshPath.empty()
        u32 MeshVertexCount = 0;
        u32 MeshIndexCount = 0;
        FoliageBoundsProfile BoundsProfile{};
        f32 MeshViewDistance = 0.0f;
        f32 MeshFadeStartDistance = 0.0f;

        u32 InstanceCount = 0;
        u32 InstanceCapacity = 0;
        u32 IndexCount = 0;

        // ── GPU cull state (issue #1235) ─────────────────────────────
        // Group bounds + the row -> group table, rebuilt only when the
        // registry generation moves.
        FoliageGPUCuller::LayerResources CullLayer;
        // One set per view slot: the compacted stream, its state and
        // indirect buffers, and the vertex arrays that stream it. The
        // arrays are per slot because each slot compacts into its OWN
        // buffer, and a vertex array names the buffer it streams.
        struct CullViewSlot
        {
            FoliageGPUCuller::ViewResources Resources;
            Ref<VertexArray> CardVAO;
            Ref<VertexArray> MeshVAO;
            // The cull this frame produced something drawable. Cleared
            // before every dispatch, so a slot that fell back reports it
            // rather than replaying the last successful frame's set --
            // the latched-flag bug TerrainGPUQuadtree::HasDispatched
            // documents.
            bool Active = false;
        };
        std::array<CullViewSlot, FoliageGPUCuller::kViewSlotCount> CullViews;
        f32 ViewDistance = 100.0f;
        f32 FadeStartDistance = 80.0f;
        f32 WindStrength = 0.3f;
        f32 WindSpeed = 1.0f;
        glm::vec4 WindWeights{ 0.0f };
        f32 InteractionResponse = 1.0f;
        glm::vec3 BaseColor{ 1.0f };
        f32 AlphaCutoff = 0.5f;
        Ref<Texture2D> AlbedoTexture;
        FString LoadedAlbedoPath; // What AlbedoTexture was opened from

        // The leaf material (issue #1234). The maps are cached BY PATH —
        // `Loaded*Path` records what each Ref was opened from, so editing
        // the path in the inspector re-opens it and NOT editing it does not
        // re-open anything. The albedo above follows the same rule.
        Ref<Texture2D> LeafNormalTexture;
        Ref<Texture2D> LeafRoughnessTexture;
        Ref<Texture2D> LeafThicknessTexture;
        FString LoadedNormalPath;
        FString LoadedRoughnessPath;
        FString LoadedThicknessPath;
        f32 LeafRoughness = 0.8f;
        f32 LeafNormalStrength = 1.0f;
        f32 LeafThickness = 0.0f;
        f32 LeafTransmissionStrength = 0.0f;
        glm::vec3 LeafTransmissionColor{ 0.42f, 0.62f, 0.18f };
        f32 LeafTransmissionDistortion = 0.35f;
        f32 LeafTransmissionPower = 4.0f;
        f32 LeafTransmissionWrap = 0.5f;
        f32 LeafTransmissionAmbient = 0.35f;

        BoundingBox Bounds; // Precomputed AABB encompassing all instances

        // Octahedral impostor (issue #433). Baked lazily from the layer mesh;
        // the *Baked* fields cache the config the atlas was baked for so a
        // regenerate only re-bakes when the mesh / grid / layout changes.
        ImpostorAtlas Impostor;
        bool UseImpostor = false;
        f32 ImpostorStartDistance = 40.0f;
        f32 ImpostorTransitionBand = 15.0f;
        FString ImpostorBakedMeshPath;
        FString ImpostorBakedAlbedoPath;
        glm::vec3 ImpostorBakedBaseColor{ 0.0f };
        f32 ImpostorBakedAlphaCutoff = 0.0f;
        u32 ImpostorBakedFrames = 0;
        u32 ImpostorBakedResolution = 0;
        bool ImpostorBakedHemi = true;

        // LOD transitions + coverage-preserving density (issue #1237).
        // SANITISED at build time (FoliageLod::Sanitise), so every consumer
        // — the three UBO-fill sites, the cull state header and the RT
        // vegetation cache — reads numbers a smoothstep can be handed. The
        // identity default is what a layer that did not author the feature
        // keeps.
        FoliageLod::Params Lod{};
    };

    // Owns TArrays, FStrings and intrusive Refs; cull resources and impostor hold external GPU objects. Bounds and remaining state are values.
    template<>
    struct TIsTriviallyRelocatable<FoliageLayerRenderData::CullViewSlot>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViewSlot::Resources)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViewSlot::CardVAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViewSlot::MeshVAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViewSlot::Active)>::Value;
    };

    // std::array has only inline element storage; this concrete array follows its audited slot type.
    template<>
    struct TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViews)>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<typename decltype(FoliageLayerRenderData::CullViews)::value_type>::Value;
    };

    template<>
    struct TIsTriviallyRelocatable<FoliageLayerRenderData>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::VAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::QuadVBO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::InstanceVBO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::IBO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshVAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshVBO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshIBO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshParts)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshRayTracingIndices)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshModel)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshGeometryPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshRequested)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshVertexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshIndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::BoundsProfile)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshViewDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::MeshFadeStartDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::InstanceCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::InstanceCapacity)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullLayer)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::CullViews)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ViewDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::FadeStartDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::WindStrength)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::WindSpeed)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::WindWeights)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::InteractionResponse)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::BaseColor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::AlphaCutoff)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::AlbedoTexture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LoadedAlbedoPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafNormalTexture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafRoughnessTexture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafThicknessTexture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LoadedNormalPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LoadedRoughnessPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LoadedThicknessPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafRoughness)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafNormalStrength)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafThickness)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionStrength)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionColor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionDistortion)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionPower)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionWrap)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::LeafTransmissionAmbient)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::Bounds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::Impostor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::UseImpostor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorStartDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorTransitionBand)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedMeshPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedAlbedoPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedBaseColor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedAlphaCutoff)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedFrames)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedResolution)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::ImpostorBakedHemi)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerRenderData::Lod)>::Value;
    };

    struct FoliageLayerDraw
    {
        Ref<VertexArray> VAO;
        u32 BaseIndex = 0;
        u32 IndexCount = 0;
        Ref<Texture2D> Albedo;
        bool IsAuthoredMesh = false;
        // The layer's mesh-to-card hand-over band, the SAME values on every
        // draw of the layer. Each draw derives the mesh's coverage fraction
        // from it and keeps the pixels the other one does not, so the two
        // partition the screen rather than overlap. Zero end = no mesh.
        f32 HandoverStart = 0.0f;
        f32 HandoverEnd = 0.0f;
        // The layer's own distance fade-out, unchanged by #1233.
        f32 FadeStart = 80.0f;
        f32 ViewDistance = 100.0f;
        // The layer's #1237 parameters, already packed into the two lanes
        // FoliageUBO::LodTransition0/1 carries. Packed ONCE, on the draw,
        // so the beauty path, the shadow path and Render() cannot pack the
        // flag bitfield three ways.
        glm::vec4 LodTransition0{ 0.0f, 30.0f, 80.0f, 0.25f };
        glm::vec4 LodTransition1{ 0.15f, 2.0f, 0.0f, 0.0f };
    };

    // External vertex-array/texture Refs with scalar and glm draw parameters.
    template<>
    struct TIsTriviallyRelocatable<FoliageLayerDraw>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FoliageLayerDraw::VAO)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::BaseIndex)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::Albedo)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::IsAuthoredMesh)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::HandoverStart)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::HandoverEnd)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::FadeStart)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::ViewDistance)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::LodTransition0)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FoliageLayerDraw::LodTransition1)>::Value;
    };

    class FoliageRenderer : public RefCounted
    {
      public:
        FoliageRenderer() = default;
        // Releases every live layer's impostor VRAM budget claim (issue
        // #718) — ImpostorAtlas has no destructor of its own (its BudgetNode
        // is a plain accounting handle, not a GPU resource, so giving it RAII
        // would mean hand-rolling move semantics just to avoid a double-free
        // on every copy); this is the one place that must not skip freeing
        // it, or a scene-reload cycle permanently starves the shared budget.
        ~FoliageRenderer();

        // The destructor above owns every layer's budget claim, so copying
        // would duplicate BudgetNode and double-free (or silently steal) it —
        // make that impossible rather than relying on nobody ever copying a
        // FoliageRenderer by value (it's already managed via Ref<T>, so
        // nothing legitimate needs these).
        FoliageRenderer(const FoliageRenderer&) = delete;
        FoliageRenderer& operator=(const FoliageRenderer&) = delete;
        FoliageRenderer(FoliageRenderer&&) = delete;
        FoliageRenderer& operator=(FoliageRenderer&&) = delete;

        // Regenerate all instances for the given layers from terrain data.
        // Call when terrain changes (erosion, sculpting) or layer settings change.
        void GenerateInstances(
            const TArray<FoliageLayer>& layers,
            const TerrainData& terrainData,
            const TerrainMaterial* material,
            f32 worldSizeX, f32 worldSizeZ, f32 heightScale);

        // The whole system stopped drawing — the component was disabled, or its
        // last layer was removed. Retires every canonical instance rather than
        // leaving records that claim plants exist (issue #1230); ids are not
        // reused afterwards. Idempotent, so Scene can call it unconditionally.
        void ClearInstances();

        // Render all visible foliage layers (frustum culled per-chunk groups)
        void Render(
            const Frustum& frustum,
            const glm::vec3& cameraPos,
            const Ref<Shader>& shader);

        // Render shadow depth pass for all layers.
        //
        // `shadowViewIndex` is this cascade's / atlas entry's item index in the
        // region. It selects the cull slot DispatchShadowViewCulling filled for
        // it; a slot that was never culled (index past kMaxShadowViews, or no
        // cull at all) draws every generated instance, which is a correct frame
        // and a slower one.
        //
        // READS ONLY, and that is load-bearing: this runs inside
        // ShadowRenderPass's RecordParallel region, where a buffer written by
        // two items is a hard Vulkan error. Every write happens earlier, in
        // DispatchShadowViewCulling.
        void RenderShadows(const Ref<Shader>& depthShader, f32 time, u32 shadowViewIndex) const;

        // Cull ONE shadow view into its own slot. Call once per active view,
        // BEFORE the region's parallel recording starts; `shadowViewIndex` is
        // the item index. Returns false when the index is past kMaxShadowViews
        // or nothing could be culled -- that view then draws uncompacted.
        bool DispatchShadowViewCulling(u32 shadowViewIndex, const FoliageGPUCuller::ViewInputs& cullInputs);

        // Retire every shadow slot's result. Call at the top of a shadow region,
        // before its per-view culls: a slot left Active from the PREVIOUS region
        // (or the previous frame) would have a later cascade draw the plants some
        // other light could see.
        void ResetShadowViewCulling();

        // ── GPU patch + instance culling (issue #1235) ────────────────────

        // Dispatch the MAIN view's cull. Called at submission, before
        // GetActiveLayerDrawInfo, so the compacted buffers and indirect commands
        // exist by the time the foliage pass replays its bucket.
        //
        // `maxDistanceScale` is not a parameter: each layer's own ViewDistance
        // is the cutoff, because that is the distance its shaders fade it out
        // at, and a second authored number would be a second thing to keep in
        // step.
        void DispatchMainViewCulling(const glm::mat4& worldViewProjection, const glm::vec3& viewWorldPosition);

        // Terrain-local cull inputs for a view given its WORLD view-projection
        // and the MAIN view's world position. Exposed because ShadowRenderPass
        // has to build the light's, and the terrain transform that makes the
        // conversion possible lives here.
        [[nodiscard]] FoliageGPUCuller::ViewInputs MakeCullInputs(const glm::mat4& worldViewProjection,
                                                                  const glm::vec3& mainViewWorldPosition) const;

        // The A/B lever for the dense-scene timing the issue's third criterion
        // asks for, and the switch a capability failure flips. Process-wide
        // rather than per renderer: the comparison it exists for is "this frame
        // with culling vs this frame without", and a per-object flag would make
        // that depend on which terrain entity you happened to select.
        static void SetGPUCullingEnabled(bool enabled);
        [[nodiscard]] static bool IsGPUCullingEnabled();

        // Forwards to FoliageGPUCuller::SetDebugOutputCapacity -- see there for
        // why the overflow path is exercised by genuinely truncating rather than
        // by faking the flag.
        void SetDebugCullCapacity(u32 entries);

        // True when the last main-view dispatch produced a compacted draw for at
        // least one layer. Deliberately not latched: a frame that fell back must
        // report that it fell back.
        [[nodiscard]] bool WasMainViewCulled() const
        {
            return m_MainViewCulled;
        }
        // Physical layer slots this renderer holds, including ones with no
        // instances. Indexes ReadbackCull, and is the space the draw info's
        // LayerIndex lives in.
        [[nodiscard]] u32 GetLayerCount() const
        {
            return static_cast<u32>(m_Layers.Num());
        }

        // Read one layer's cull result back to the CPU. STALLS -- see
        // FoliageGPUCuller::Readback. False when that slot has no live cull.
        [[nodiscard]] bool ReadbackCull(u32 layerIndex, FoliageGPUCuller::ViewSlot slot,
                                        FoliageGPUCuller::Readback& out) const;

        // The owning terrain entity's world transform. GenerateInstances emits
        // instance positions in TERRAIN-LOCAL space (x/z in [0, WorldSize], y
        // the raw sampled height in [0, HeightScale] — no base offset), exactly
        // like the terrain mesh itself, so something has to place them.
        //
        // The MAIN draw gets that transform from its command packet
        // (Scene passes it to Renderer3D::DrawFoliageLayer, and
        // CommandDispatch::DrawFoliageLayer uploads it as the shaders' single
        // `u_Model` entry). RenderShadows has no command packet — ShadowRenderPass
        // drives it straight off this object — so it needs the transform stored
        // here, and it was uploading plain identity instead, casting every
        // island's foliage shadow from a heap of plants at the world origin
        // (issue #953). Render() below uploads it too, for when it regains a
        // caller; it currently has none.
        //
        // Kept as a MATRIX rather than baked into the instance positions so a
        // terrain that moves takes its foliage with it, with no rebuild.
        void SetTerrainTransform(const glm::mat4& transform)
        {
            m_TerrainTransform = transform;
            // The canonical records are terrain-local, so a terrain that moves
            // invalidates no identity (issue #1230) -- only the groups' cached
            // world bounds change, and the registry no-ops when the matrix is
            // unchanged, which is the common case since this runs every frame.
            m_Registry.SetTerrainTransform(transform);
        }

        // Canonical per-instance identity, spatial groups and the
        // represented-vs-unsupported census (issue #1230). The instance VBO is
        // a PROJECTION of these records; nothing may key state on a buffer row.
        [[nodiscard]] const FoliageInstanceRegistry& GetInstanceRegistry() const
        {
            return m_Registry;
        }

        [[nodiscard]] u32 GetTotalInstanceCount() const;
        [[nodiscard]] u32 GetVisibleInstanceCount() const
        {
            return m_VisibleInstances;
        }

        // Returns draw info for all active layers (InstanceCount > 0 && VAO valid).
        // Used by Scene to create DrawFoliageLayerCommand packets per layer.
        [[nodiscard]] TArray<FoliageLayerDrawInfo> GetActiveLayerDrawInfo() const;
        void QueueRayTracing(u64 owner, const glm::vec3& cameraPosition) const;

        void SetTime(f32 time, f32 prevTime)
        {
            m_WindHistory.Advance(time, prevTime);
            m_Time = m_WindHistory.Time;
            m_PrevTime = m_WindHistory.PreviousTime;
        }

        bool SetLegacyWindEnvelope(f32 envelope)
        {
            const bool changed = !Math::BitwiseEqual(m_LegacyWindEnvelope, envelope);
            m_LegacyWindEnvelope = envelope;
            return changed;
        }

        [[nodiscard]] f32 GetPreviousTime() const
        {
            return m_PrevTime;
        }

      private:
        // One drawable index range of a layer's geometry, with the material it
        // is drawn with (issue #1233). The card is a single part; an authored
        // mesh contributes one part per submesh so a plant whose trunk and
        // leaves use different textures renders as authored.
        using LayerDrawPart = FoliageLayerDrawPart;

        // Internal per-layer GPU data
        using LayerRenderData = FoliageLayerRenderData;

        // ONE draw this layer contributes: an index range of one of its vertex
        // arrays, the material it is drawn with, and the distance band it owns.
        //
        // THE list, walked by all three consumers — the beauty/G-Buffer
        // submission path (GetActiveLayerDrawInfo), the shadow pass
        // (RenderShadows) and the direct Render() path. That is what makes
        // issue #1233's fourth criterion structural rather than reviewable: a
        // pass cannot draw a quad where another drew a pine, because none of
        // them decides what to draw.
        using LayerDraw = FoliageLayerDraw;
        void EnumerateLayerDraws(const LayerRenderData& data, TArray<LayerDraw>& out) const;

        // Run one view's cull over every layer. Returns true when at least one
        // layer produced a compacted draw.
        bool CullForView(u32 slotIndex, const FoliageGPUCuller::ViewInputs& inputs);
        // (Re)create the vertex arrays that stream `slot`'s compacted buffer.
        void RebuildCulledVertexArrays(LayerRenderData& data, u32 slot) const;

        void BuildQuadGeometry(LayerRenderData& data) const;
        // Builds (or rebuilds) the layer's private copy of the authored mesh.
        // Returns false and logs loudly when the mesh will not load — the layer
        // then draws its card everywhere, which is the pre-#1233 look, and the
        // registry counts the variant as unavailable. Never a silent fallback.
        bool BuildMeshGeometry(LayerRenderData& data, const FoliageLayer& layer) const;
        // (Re)creates the vertex arrays over whatever geometry and instance
        // buffers the layer currently holds. Split out because the instance VBO
        // has to be bound into EVERY vertex array the layer draws from, and a
        // capacity grow replaces it.
        void RebuildVertexArrays(LayerRenderData& data) const;
        void UploadInstances(LayerRenderData& data, const TArray<FoliageInstanceData>& instances);

        // Bakes (or re-bakes) the layer's octahedral impostor atlas if UseImpostor
        // and the mesh/grid/layout differs from what was last baked. No-op otherwise.
        void UpdateImpostorAtlas(LayerRenderData& data, const FoliageLayer& layer);

        TArray<LayerRenderData> m_Layers;
        FoliageInstanceRegistry m_Registry;
        FoliageGPUCuller m_Culler;
        bool m_MainViewCulled = false;
        // Warn-once latches: a capability failure or a per-layer refusal must
        // say so, but a renderer that says it every frame at 60 Hz is the same
        // as saying nothing (35k lines in one session -- see the GLStateGuard
        // note in docs/agent-rules).
        mutable bool m_WarnedCullUnavailable = false;
        bool m_WarnedShadowViewOverflow = false;
        bool m_WarnedTooManyParts = false;
        glm::mat4 m_TerrainTransform{ 1.0f };
        u32 m_VisibleInstances = 0;
        f32 m_LegacyWindEnvelope = 2.0f;
        Ref<Shader> m_ImpostorDepthShader;
        FoliageWindHistory m_WindHistory;
        f32 m_Time = 0.0f;
        f32 m_PrevTime = 0.0f;
    };
} // namespace OloEngine

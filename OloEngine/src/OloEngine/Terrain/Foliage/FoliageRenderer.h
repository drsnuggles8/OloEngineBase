#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Terrain/Foliage/FoliageInstanceRegistry.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Renderer/Model.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

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
        u32 InstanceCount = 0;
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
        glm::vec3 BaseColor{ 1.0f };
        f32 AlphaCutoff = 0.5f;
        BoundingBox Bounds; // Precomputed AABB encompassing all instances in this layer

        // Octahedral impostor LOD (issue #433). UseImpostor + valid atlas IDs
        // route this layer through the impostor card shader instead of the flat
        // billboard; zero/false leaves the existing billboard path untouched.
        bool UseImpostor = false;
        RHI::ResourceHandle ImpostorAlbedoAtlasID{};
        RHI::ResourceHandle ImpostorNormalDepthAtlasID{};
        u32 ImpostorFramesPerAxis = 8;
        bool ImpostorHemi = true;
        f32 ImpostorStartDistance = 40.0f;
        f32 ImpostorTransitionBand = 15.0f;
        f32 ImpostorRadius = 1.0f;
    };

    // Manages foliage instance generation, culling, and instanced rendering.
    // Generates instances on the CPU from terrain data + foliage layer config,
    // uploads to a per-layer instance VBO, and draws with DrawIndexedInstanced.
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
            const std::vector<FoliageLayer>& layers,
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

        // Render shadow depth pass for all layers
        void RenderShadows(const Ref<Shader>& depthShader, f32 time) const;

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
        [[nodiscard]] std::vector<FoliageLayerDrawInfo> GetActiveLayerDrawInfo() const;

        void SetTime(f32 time, f32 prevTime)
        {
            m_Time = time;
            m_PrevTime = prevTime;
        }

      private:
        // One drawable index range of a layer's geometry, with the material it
        // is drawn with (issue #1233). The card is a single part; an authored
        // mesh contributes one part per submesh so a plant whose trunk and
        // leaves use different textures renders as authored.
        struct LayerDrawPart
        {
            u32 BaseIndex = 0;
            u32 IndexCount = 0;
            // The submesh's own albedo. Null falls through to the layer's
            // AlbedoTexture, which is what a mesh with no imported texture gets.
            Ref<Texture2D> Albedo;
        };

        // Internal per-layer GPU data
        struct LayerRenderData
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
            std::vector<LayerDrawPart> MeshParts;
            // Keeps the imported materials' textures alive for as long as the
            // parts reference them.
            Ref<Model> MeshModel;
            std::string MeshGeometryPath; // What MeshVBO/MeshIBO were built from
            bool MeshRequested = false;   // UseAuthoredMesh && !MeshPath.empty()
            u32 MeshVertexCount = 0;
            u32 MeshIndexCount = 0;
            FoliageBoundsProfile BoundsProfile{};
            f32 MeshViewDistance = 0.0f;
            f32 MeshFadeStartDistance = 0.0f;

            u32 InstanceCount = 0;
            u32 InstanceCapacity = 0;
            u32 IndexCount = 0;
            f32 ViewDistance = 100.0f;
            f32 FadeStartDistance = 80.0f;
            f32 WindStrength = 0.3f;
            f32 WindSpeed = 1.0f;
            glm::vec3 BaseColor{ 1.0f };
            f32 AlphaCutoff = 0.5f;
            Ref<Texture2D> AlbedoTexture;
            BoundingBox Bounds; // Precomputed AABB encompassing all instances

            // Octahedral impostor (issue #433). Baked lazily from the layer mesh;
            // the *Baked* fields cache the config the atlas was baked for so a
            // regenerate only re-bakes when the mesh / grid / layout changes.
            ImpostorAtlas Impostor;
            bool UseImpostor = false;
            f32 ImpostorStartDistance = 40.0f;
            f32 ImpostorTransitionBand = 15.0f;
            std::string ImpostorBakedMeshPath;
            std::string ImpostorBakedAlbedoPath;
            glm::vec3 ImpostorBakedBaseColor{ 0.0f };
            f32 ImpostorBakedAlphaCutoff = 0.0f;
            u32 ImpostorBakedFrames = 0;
            u32 ImpostorBakedResolution = 0;
            bool ImpostorBakedHemi = true;
        };

        // ONE draw this layer contributes: an index range of one of its vertex
        // arrays, the material it is drawn with, and the distance band it owns.
        //
        // THE list, walked by all three consumers — the beauty/G-Buffer
        // submission path (GetActiveLayerDrawInfo), the shadow pass
        // (RenderShadows) and the direct Render() path. That is what makes
        // issue #1233's fourth criterion structural rather than reviewable: a
        // pass cannot draw a quad where another drew a pine, because none of
        // them decides what to draw.
        struct LayerDraw
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
        };
        void EnumerateLayerDraws(const LayerRenderData& data, std::vector<LayerDraw>& out) const;

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
        void UploadInstances(LayerRenderData& data, const std::vector<FoliageInstanceData>& instances);

        // Bakes (or re-bakes) the layer's octahedral impostor atlas if UseImpostor
        // and the mesh/grid/layout differs from what was last baked. No-op otherwise.
        void UpdateImpostorAtlas(LayerRenderData& data, const FoliageLayer& layer);

        std::vector<LayerRenderData> m_Layers;
        FoliageInstanceRegistry m_Registry;
        glm::mat4 m_TerrainTransform{ 1.0f };
        u32 m_VisibleInstances = 0;
        f32 m_Time = 0.0f;
        f32 m_PrevTime = 0.0f;
    };
} // namespace OloEngine

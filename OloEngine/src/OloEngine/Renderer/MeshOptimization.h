#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/LOD.h"

#include <vector>

namespace OloEngine
{
    class MeshSource;
    struct Vertex;

    // Mesh analysis results from meshoptimizer diagnostic functions.
    struct MeshAnalysis
    {
        // Vertex cache statistics
        f32 ACMR = 0.0f; // Average Cache Miss Ratio (lower = better; ideal < 0.7)
        f32 ATVR = 0.0f; // Average Transform-to-Vertex Ratio (lower = better; ideal ~1.0)
        // Overdraw estimate
        f32 Overdraw = 0.0f; // Overdraw ratio (1.0 = no overdraw; higher = worse)
        // Vertex fetch efficiency
        f32 OverfetchRatio = 0.0f; // Bytes fetched / bytes needed (1.0 = optimal)
        // Mesh totals
        u32 VertexCount = 0;
        u32 TriangleCount = 0;
    };

    // Degenerate-triangle census for a mesh (issue #629).
    //
    // A UV-DEGENERATE triangle has zero area in TEXTURE space but non-zero area in 3D:
    // its three corners share a texcoord (or are collinear in UV), so the interpolated UV
    // is constant along at least one axis and the screen-space UV derivative used to build
    // a tangent frame collapses. `normalize(vec3(0))` is NaN, and a NaN tangent poisons the
    // G-Buffer normal — Sponza's potted vines rendered a white lacework because of exactly
    // this. The shader guards it (getNormalFromMap in PBRCommon.glsl falls back to the
    // geometric normal), but the data is still junk, so it is worth SEEING at import.
    //
    // These triangles are NOT dropped: they carry real 3D area (in Sponza, two whole
    // submeshes are 100% UV-degenerate), so removing them would punch holes in the mesh.
    struct DegenerateTriangleStats
    {
        u32 TriangleCount = 0;     // Total triangles examined
        u32 ZeroAreaCount = 0;     // Zero 3D area — genuinely dead geometry (Assimp's
                                   // aiProcess_FindDegenerates normally removes these)
        u32 ZeroUvAreaCount = 0;   // Zero UV area BUT non-zero 3D area — real, untextured geometry
        f64 ZeroUvArea3DSum = 0.0; // Total 3D area those UV-degenerate triangles cover

        [[nodiscard]] bool HasDegenerates() const
        {
            return ZeroAreaCount > 0 || ZeroUvAreaCount > 0;
        }
    };

    // Meshlet data for GPU-driven mesh rendering (mesh shader pipeline).
    struct MeshletData
    {
        struct Meshlet
        {
            u32 VertexOffset = 0;
            u32 TriangleOffset = 0;
            u32 VertexCount = 0;
            u32 TriangleCount = 0;
        };

        struct MeshletBounds
        {
            f32 Center[3] = {};
            f32 Radius = 0.0f;
            f32 ConeApex[3] = {};
            f32 ConeAxis[3] = {};
            f32 ConeCutoff = 0.0f;
        };

        std::vector<Meshlet> Meshlets;
        std::vector<u32> MeshletVertices;  // Vertex indices for all meshlets
        std::vector<u8> MeshletTriangles;  // Local triangle indices (3 bytes per tri)
        std::vector<MeshletBounds> Bounds; // Per-meshlet bounding cone for GPU culling
    };

    // Encoded mesh buffer for compact asset pack storage.
    struct EncodedMeshBuffer
    {
        std::vector<u8> Data;
        sizet OriginalSize = 0; // Original unencoded size in bytes
    };

    // Mesh optimization utilities powered by meshoptimizer.
    // These operate on CPU-side vertex/index data before GPU upload.
    namespace MeshOptimization
    {
        // ── Core optimization ──

        // Optimizes triangle order for GPU post-transform vertex cache,
        // reduces overdraw, and reorders vertices for sequential access.
        // Also generates shadow index buffer for depth-only passes.
        void OptimizeMesh(MeshSource& meshSource);

        // ── LOD generation ──

        // Generates a simplified mesh with the target triangle ratio [0,1].
        Ref<MeshSource> GenerateLODMesh(const MeshSource& meshSource, f32 targetRatio, f32 targetError = 0.01f);

        // Attribute-aware simplification that preserves UV/normal quality.
        // Produces better LODs for textured meshes at the cost of more CPU time.
        Ref<MeshSource> GenerateLODMeshWithAttributes(const MeshSource& meshSource, f32 targetRatio, f32 targetError = 0.01f);

        // Generates a complete LODGroup with multiple simplified levels, spacing the
        // authored MaxDistance thresholds evenly over `maxDistance`.
        //
        // Prefer GenerateAutoLODGroup below for new work (issue #711): this one
        // re-simplifies LOD 0 at a fixed ratio per level and produces no error
        // measure, so its levels can only be selected by hand-tuned distance.
        LODGroup GenerateLODGroup(const MeshSource& meshSource, AssetHandle baseMeshHandle, u32 lodCount = 4, f32 maxDistance = 200.0f);

        // Tuning for GenerateAutoLODGroup. The defaults produce the 8-12 levels a
        // typical mesh supports before the error measure stops it.
        struct AutoLODSettings
        {
            // Ceiling on the number of levels INCLUDING LOD 0. The chain normally
            // ends on the error / triangle-floor conditions well before this.
            u32 MaxLevels = 12;
            // Stop once a level would fall below this many triangles — below it the
            // silhouette is gone and further levels buy nothing.
            u32 MinTriangleCount = 32;
            // Stop when one simplification step's relative error exceeds this.
            // Relative to the model's largest bounding-box extent, like meshopt's.
            f32 MaxStepError = 0.5f;
            // How much a unit of normal distortion counts against a unit of
            // positional distortion. Normals drive shading, so they are weighted
            // slightly harder than geometry; see the notes in the .cpp for why the
            // weight is then scaled by the level's average vertex distance.
            f32 NormalImportance = 3.0f;
            // UVs get a small weight purely to stop the simplifier from erasing
            // mirrored/atlassed UV seams it sees no geometric value in.
            f32 TexCoordWeight = 1.0f;
        };

        // One generated level: the simplified geometry plus the measurement that
        // produced it. Index 0 of a chain is the unmodified source, error 0.
        struct AutoLODChainEntry
        {
            Ref<MeshSource> Source; // null for entry 0 — that IS the source mesh
            u32 TriangleCount = 0;
            f32 Error = 0.0f; // accumulated, relative to the model extent
        };

        // The CPU half of automatic LOD generation: simplifies, measures, and stops.
        // Touches no AssetManager and uploads nothing to the GPU, which is what makes
        // the error metric testable without a project or a graphics device.
        //
        // Always returns at least entry 0. Entries after it are ordered fine -> coarse
        // with strictly increasing Error.
        std::vector<AutoLODChainEntry> BuildAutoLODChain(const MeshSource& meshSource,
                                                         const AutoLODSettings& settings = {});

        // The largest bounding-box side over the vertices `meshSource`'s indices
        // reference — the unit AutoLODChainEntry::Error is expressed in. Returns 0 for
        // an empty or degenerate mesh.
        [[nodiscard]] f32 MeasureModelExtent(const MeshSource& meshSource);

        // Generates a LODGroup by repeatedly halving the PREVIOUS level's triangle
        // count and recording each level's accumulated error (issue #711).
        //
        // Unlike GenerateLODGroup this needs no authored distances: every level
        // carries a `LODLevel::Error` in model-extent units, which
        // LODGroup::SelectLODByPixelError turns into an estimated pixel error at
        // runtime. MaxDistance is still filled in, derived from the same errors, so
        // the group stays usable on the legacy distance path and readable in the
        // inspector.
        //
        // Returns a group holding only LOD 0 when the source cannot be simplified
        // (multi-submesh, skinned, or morph-target sources are rejected outright).
        LODGroup GenerateAutoLODGroup(const MeshSource& meshSource, AssetHandle baseMeshHandle,
                                      const AutoLODSettings& settings = {});

        // ── Shadow rendering ──

        // Generates a position-only shadow index buffer that merges vertices
        // sharing the same position. Stored in MeshSource for depth-only passes.
        void GenerateShadowIndices(MeshSource& meshSource);

        // ── Mesh analysis ──

        // Returns vertex cache, overdraw, and fetch statistics for a mesh.
        MeshAnalysis AnalyzeMesh(const MeshSource& meshSource);

        // Counts degenerate triangles (zero 3D area, and zero-UV-area-but-real-geometry).
        // Pure analysis — changes nothing. Called by OptimizeMesh at import so a bad asset
        // logs a warning instead of silently costing every covered fragment a NaN-guard.
        DegenerateTriangleStats AnalyzeDegenerateTriangles(const MeshSource& meshSource);

        // Same census over a raw vertex/index span — used by the cluster-DAG builder to
        // report how many UV-degenerate triangles SIMPLIFICATION creates on top of the ones
        // the source mesh already ships.
        DegenerateTriangleStats AnalyzeDegenerateTriangles(const Vertex* vertices, sizet vertexCount,
                                                           const u32* indices, sizet indexCount);

        // ── Meshlet generation ──

        // Builds meshlets suitable for mesh shader / GPU-driven rendering.
        // maxVertices: max vertices per meshlet (typically 64).
        // maxTriangles: max triangles per meshlet (typically 124, must be <= 512).
        MeshletData GenerateMeshlets(const MeshSource& meshSource, u32 maxVertices = 64, u32 maxTriangles = 124);

        // ── Spatial optimization ──

        // Reorders triangles for better spatial locality (improves
        // raytracing and occlusion culling query coherence).
        void SpatialSortTriangles(MeshSource& meshSource);

        // ── Buffer encoding for asset packs ──

        // Encodes vertex data into a compact binary form (~50-75% smaller).
        EncodedMeshBuffer EncodeVertexBuffer(const void* vertices, sizet vertexCount, sizet vertexSize);
        bool DecodeVertexBuffer(void* destination, sizet vertexCount, sizet vertexSize, const EncodedMeshBuffer& encoded);

        // Encodes index data into a compact binary form.
        EncodedMeshBuffer EncodeIndexBuffer(const u32* indices, sizet indexCount, sizet vertexCount);
        bool DecodeIndexBuffer(u32* destination, sizet indexCount, const EncodedMeshBuffer& encoded);
    } // namespace MeshOptimization
} // namespace OloEngine

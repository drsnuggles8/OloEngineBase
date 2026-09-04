#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    class TerrainData : public Asset
    {
      public:
        TerrainData() = default;
        ~TerrainData() override = default;

        // Load heightmap from a 16-bit PNG or raw R32F file
        bool LoadFromFile(const std::string& path);

        // Create a flat heightmap of given resolution
        void CreateFlat(u32 resolution, f32 defaultHeight = 0.0f);

        // Generate procedural terrain using fBm simplex noise
        // seed: random seed, octaves: detail layers, frequency: base scale, amplitude: height variation
        // lacunarity: frequency multiplier per octave, persistence: amplitude multiplier per octave
        void GenerateProcedural(u32 resolution, i32 seed, u32 octaves = 6,
                                f32 frequency = 2.0f, f32 amplitude = 1.0f,
                                f32 lacunarity = 2.0f, f32 persistence = 0.5f);

        // Replace the heightmap wholesale with an externally generated field
        // (row-major, resolution × resolution, expected in [0, 1]). Re-uploads
        // to the GPU. Used by TerrainGenerator to push a procedurally shaped
        // height field without GenerateProcedural's fixed single-fBm formula.
        void SetHeights(u32 resolution, std::vector<f32> heights);

        // CPU height query with bilinear interpolation — normalizedX/Z in [0, 1]
        [[nodiscard]] f32 GetHeightAt(f32 normalizedX, f32 normalizedZ) const;

        // CPU normal query from finite differences
        [[nodiscard]] glm::vec3 GetNormalAt(f32 normalizedX, f32 normalizedZ, f32 worldSizeX, f32 worldSizeZ, f32 heightScale) const;

        // The same two queries against a RAW height field, with no TerrainData
        // (and therefore no GL context) in the way. SetHeights() uploads to the
        // GPU, so a headless caller — the auto-material coverage tests, any
        // tool reasoning about a field it just generated — cannot go through the
        // members above. The members delegate here, so there is exactly one
        // implementation of the sampling convention and the slope metric that
        // TerrainGenerator::GenerateSplatmap classifies layers by.
        [[nodiscard]] static f32 SampleHeight(const std::vector<f32>& heights, u32 resolution,
                                              f32 normalizedX, f32 normalizedZ);
        [[nodiscard]] static glm::vec3 SampleNormal(const std::vector<f32>& heights, u32 resolution,
                                                    f32 normalizedX, f32 normalizedZ, f32 worldSizeX,
                                                    f32 worldSizeZ, f32 heightScale);
        // Slope in DEGREES from the surface normal — the quantity a
        // TerrainLayerRule's MinSlopeDeg/MaxSlopeDeg band is expressed in.
        [[nodiscard]] static f32 SampleSlopeDegrees(const std::vector<f32>& heights, u32 resolution,
                                                    f32 normalizedX, f32 normalizedZ, f32 worldSizeX,
                                                    f32 worldSizeZ, f32 heightScale);

        [[nodiscard]] u32 GetResolution() const
        {
            return m_Resolution;
        }

        // ── Height-content revision (issue #1033) ──
        //
        // Bumped whenever the height SAMPLES change, as opposed to the vector
        // that holds them. Consumers that cache something derived from this
        // field need it because the obvious identity — the heights vector's
        // address and size — does not move when the contents do: SyncFromGPU()
        // refreshes an already-correctly-sized mirror with resize() + memcpy, so
        // a GPU sculpt rewrites every sample at the same address, and a CPU brush
        // stroke through the non-const GetHeightData() does the same.
        //
        // A cache keyed on the address alone therefore looks valid forever while
        // the terrain visibly changes underneath it. WaterShoreDepthSystem's
        // seabed field was exactly that: the surf line would keep shoaling
        // against the coastline the terrain had when the scene loaded.
        //
        // It is a counter and not a dirty flag on purpose — a flag cannot
        // distinguish "edited, consumed, edited again" from "not edited".
        [[nodiscard]] u64 GetHeightRevision() const
        {
            return m_HeightRevision;
        }
        // ── The CPU/GPU height sync point (issue #716) ──
        //
        // Since the sculpt brush and hydraulic erosion became GPU-resident, the
        // GPU heightmap — not m_Heights — is the source of truth during
        // authoring. m_Heights is a MIRROR, refreshed lazily here and nowhere
        // else, which is what lets the authoring path run without a per-operation
        // readback while every CPU consumer (physics height field, quadtree
        // pyramid, chunk rebuild, tile stitching, save/export, height queries)
        // keeps reading a correct field.
        //
        // Both accessors sync, so a consumer cannot silently read a stale mirror:
        // that failure mode is invisible — the terrain looks right and gameplay
        // disagrees with it — which is precisely why the sync is here rather than
        // left to each of the twelve call sites to remember.
        [[nodiscard]] const std::vector<f32>& GetHeightData() const
        {
            SyncFromGPU();
            return m_Heights;
        }
        // The non-const form hands out a writable mirror, so the CPU becomes
        // authoritative again the moment a caller uses it; the caller still owes
        // the matching UploadToGPU / UploadRegionToGPU, exactly as before.
        [[nodiscard]] std::vector<f32>& GetHeightData()
        {
            SyncFromGPU();
            return m_Heights;
        }

        // Declare the GPU copy newer than the CPU mirror. Called by the GPU brush
        // and by erosion after their dispatches; the next CPU consumer pays for
        // one readback, and a consumer-free stroke pays for none at all.
        void MarkGPUModified()
        {
            m_CPUMirrorStale = true;
        }

        // Pull the GPU heightmap back into the CPU mirror if it is stale. This is
        // the ONE readback left on the terrain authoring path — const because
        // refreshing a mirror is not a logical mutation, and every read accessor
        // above needs to be able to call it.
        void SyncFromGPU() const;

        [[nodiscard]] bool IsCPUMirrorStale() const
        {
            return m_CPUMirrorStale;
        }
        [[nodiscard]] Ref<Texture2D> GetGPUHeightmap() const
        {
            return m_GPUHeightmap;
        }

        // Re-upload full heightmap to GPU (call after CPU edits)
        void UploadToGPU();

        // Re-upload a rectangular region to GPU (partial update for brush editing)
        void UploadRegionToGPU(u32 x, u32 y, u32 width, u32 height);

        // Export heightmap to file
        // R32F raw format: direct float array dump (resolution × resolution × 4 bytes)
        bool ExportRawR32F(const std::string& path) const;
        // R16 raw format: quantized to 16-bit unsigned (resolution × resolution × 2 bytes)
        bool ExportRawR16(const std::string& path) const;

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Terrain;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

      private:
        u32 m_Resolution = 0; // Heightmap is m_Resolution × m_Resolution
        // Mutable because SyncFromGPU() refreshes them from a const read
        // accessor — see the sync-point comment above.
        mutable std::vector<f32> m_Heights; // Row-major CPU MIRROR of the heightmap, [0, 1]
        // Bumped at the three points height CONTENT changes — see
        // GetHeightRevision(). Mutable because SyncFromGPU() is const.
        mutable u64 m_HeightRevision = 0;
        mutable bool m_CPUMirrorStale = false;
        Ref<Texture2D> m_GPUHeightmap; // R32F GPU texture — authoritative while authoring
    };
} // namespace OloEngine

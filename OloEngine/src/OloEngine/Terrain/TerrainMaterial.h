#pragma once

#include "OloEngine/Containers/Array.h"

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainLayer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture2DArray.h"

#include <array>
#include <span>
#include "OloEngine/Containers/String.h"

namespace OloEngine
{
    // The three per-layer texture arrays of a terrain material: albedo (sRGB),
    // normal and ARM (both linear), one slice per layer.
    //
    // Its own object so that materials with the same layers SHARE one set
    // (issue #1721): a scene of six islands painted from the same eight layer
    // files used to hold six copies of every array. TerrainMaterial keeps a
    // weak cache keyed on what the arrays are built from — each layer's three
    // files (path, size and modification time), its fallback colour, roughness
    // and metallic, and the resolution — and hands out the live set when the
    // key matches. Tiling and blend sharpness are per-terrain UBO values and
    // are not part of it.
    struct TerrainLayerArrays : public RefCounted
    {
        Ref<Texture2DArray> Albedo;
        Ref<Texture2DArray> Normal;
        Ref<Texture2DArray> ARM;
        u32 Resolution = 0;
    };

    // Manages terrain material layers and their GPU texture arrays.
    // Up to 8 layers supported via 2 RGBA8 splatmaps (4 channels each).
    // Packs per-layer textures into 3 Texture2DArrays: albedo, normal, ARM.
    class TerrainMaterial : public RefCounted
    {
      public:
        // Per-layer array resolution: a power of two in [kMin, kMax]. 512 is
        // what every material was built at before the value was authorable.
        static constexpr u32 kDefaultLayerResolution = 512;
        static constexpr u32 kMinLayerResolution = 64;
        static constexpr u32 kMaxLayerResolution = 4096;
        // Anisotropic filtering degree asked of the layer arrays; each backend
        // clamps it to what the device offers.
        static constexpr f32 kLayerAnisotropy = 16.0f;

        TerrainMaterial() = default;

        // Add a layer (returns layer index, or -1 if full)
        i32 AddLayer(const TerrainLayer& layer);

        // Remove a layer by index (shifts subsequent layers down)
        void RemoveLayer(u32 index);

        // Get/set layer at index
        [[nodiscard]] const TerrainLayer& GetLayer(u32 index) const;
        TerrainLayer& GetLayer(u32 index);

        [[nodiscard]] u32 GetLayerCount() const
        {
            return m_LayerCount;
        }

        // Build GPU texture arrays from layer textures, at the material's layer
        // resolution. Must be called after adding/modifying layers before
        // rendering. A material whose layers match an already built one takes
        // that one's arrays instead of building its own (see TerrainLayerArrays).
        void BuildTextureArrays();
        // The same after SetLayerResolution(layerResolution).
        void BuildTextureArrays(u32 layerResolution);

        // The nearest valid layer resolution at or below `requested`: clamped
        // to [kMinLayerResolution, kMaxLayerResolution] and rounded DOWN to a
        // power of two.
        [[nodiscard]] static u32 SanitizeLayerResolution(u32 requested);

        // Takes effect at the next BuildTextureArrays().
        void SetLayerResolution(u32 resolution)
        {
            m_LayerResolution = SanitizeLayerResolution(resolution);
        }

        // Splatmap management.
        //
        // A splatmap FILE is weights, not colour: it is decoded on the CPU as
        // linear RGBA8, with image row 0 at terrain v = 0 (the z = 0 edge) —
        // the heightmap's and the layer textures' orientation — and the GPU
        // textures are uploaded from that copy. So the CPU mirror the paint
        // brush, the foliage masks and the save path read holds the weights the
        // shader samples. The two planes share one resolution: a plane with no
        // file is all zeros, and two files of different sizes are refused with
        // an error naming both.
        void SetSplatmapPath(u32 index, const FString& path); // index 0 or 1
        [[nodiscard]] const FString& GetSplatmapPath(u32 index) const;
        void LoadSplatmaps();

        // The macro map (issue #1721): one optional linear RGBA8 texture over
        // the terrain's own [0,1] UV, image row 0 at v = 0 like the splatmaps.
        // RGB tints the blended albedo (byte 128 is neutral), alpha is dryness
        // (255 = dry) — see Terrain/TerrainSurfaceShading.h for the math. The
        // path is a stored content path (ResolveContentPath).
        void SetMacroMapPath(const FString& path)
        {
            m_MacroMapPath = path;
        }
        [[nodiscard]] const FString& GetMacroMapPath() const
        {
            return m_MacroMapPath;
        }
        // Loads (or, with an empty path, drops) the macro map. A path that does
        // not load leaves no map and logs why.
        void LoadMacroMap();
        [[nodiscard]] Ref<Texture2D> GetMacroMap() const
        {
            return m_MacroMap;
        }

        // GPU resources for rendering
        [[nodiscard]] Ref<Texture2DArray> GetAlbedoArray() const
        {
            return m_LayerArrays ? m_LayerArrays->Albedo : nullptr;
        }
        [[nodiscard]] Ref<Texture2DArray> GetNormalArray() const
        {
            return m_LayerArrays ? m_LayerArrays->Normal : nullptr;
        }
        [[nodiscard]] Ref<Texture2DArray> GetARMArray() const
        {
            return m_LayerArrays ? m_LayerArrays->ARM : nullptr;
        }
        // The shared set itself. Two materials built from identical layers
        // return the same object.
        [[nodiscard]] Ref<TerrainLayerArrays> GetLayerArrays() const
        {
            return m_LayerArrays;
        }
        [[nodiscard]] Ref<Texture2D> GetSplatmap(u32 index) const;

        [[nodiscard]] bool IsBuilt() const
        {
            return m_LayerArrays != nullptr && m_LayerArrays->Albedo != nullptr;
        }

        // Per-layer texture-array resolution. Needed by any consumer that has to
        // pick an explicit sampling LOD instead of relying on derivatives — the
        // virtual-texture tile bake (issue #715) runs in a compute shader, which
        // has none.
        [[nodiscard]] u32 GetLayerResolution() const
        {
            return m_LayerResolution;
        }

        // ── Pure CPU helpers, public so their contracts can be tested ────────

        // Resample an RGBA8 image to dstWidth x dstHeight. Each axis is treated
        // on its own: an axis that shrinks is AREA-averaged (every source texel
        // contributes by the fraction of it a destination texel covers), an
        // axis that grows is interpolated linearly between texel centres with
        // wrap-around, because a layer texture tiles. Never a point sample.
        //
        // `srgbColour` filters the RGB channels in linear light (decode,
        // average, encode), which is what keeps a high-contrast albedo from
        // darkening when it is minified; alpha is always linear.
        static void ResampleRGBA8(std::span<const u8> source, u32 srcWidth, u32 srcHeight, u32 dstWidth,
                                  u32 dstHeight, bool srgbColour, TArray<u8>& outPixels);

        // The sRGB transfer function on one 8-bit channel. LinearToSRGB8 is
        // what turns a layer's LINEAR fallback BaseColor into the byte an sRGB
        // array stores, so that sampling it gives the colour back.
        [[nodiscard]] static u8 LinearToSRGB8(f32 linear);
        [[nodiscard]] static f32 SRGB8ToLinear(u8 encoded);

        // CPU splatmap data for paint brush editing
        void InitializeCPUSplatmaps(u32 resolution);
        [[nodiscard]] u32 GetSplatmapResolution() const
        {
            return m_SplatmapResolution;
        }
        // Both accessors sync the CPU mirror from the GPU first if the GPU paint
        // brush has moved ahead of it (issue #716) — the splatmap twin of
        // TerrainData's height sync point. Consumers that read this without a
        // sync (foliage density masks, the auto-material evidence test, save) saw
        // a pre-stroke splatmap and had no way to tell.
        [[nodiscard]] TArray<u8>& GetSplatmapData(u32 index);
        [[nodiscard]] const TArray<u8>& GetSplatmapData(u32 index) const;

        // Declare the GPU splatmaps newer than the CPU mirror. Called by
        // TerrainGPUBrush::ApplyPaint; the next CPU consumer pays for one readback.
        void MarkSplatmapsGPUModified()
        {
            m_CPUSplatmapsStale = true;
            ++m_SplatmapRevision;
        }

        [[nodiscard]] u64 GetSplatmapRevision() const
        {
            return m_SplatmapRevision;
        }

        // The one splatmap readback left on the authoring path.
        void SyncSplatmapsFromGPU() const;

        [[nodiscard]] bool AreCPUSplatmapsStale() const
        {
            return m_CPUSplatmapsStale;
        }
        void UploadSplatmapRegion(u32 splatmapIndex, u32 x, u32 y, u32 w, u32 h);
        [[nodiscard]] bool HasCPUSplatmaps() const
        {
            return m_SplatmapResolution > 0 && !m_CPUSplatmaps[0].IsEmpty();
        }

      private:
        // (Re)create both GPU splatmap textures from the CPU mirror.
        void CreateSplatmapTexturesFromCPU();

        std::array<TerrainLayer, MAX_TERRAIN_LAYERS> m_Layers;
        u32 m_LayerCount = 0;

        // Splatmaps (RGBA8): splatmap 0 = layers 0-3, splatmap 1 = layers 4-7
        std::array<FString, 2> m_SplatmapPaths;
        std::array<Ref<Texture2D>, 2> m_Splatmaps;

        FString m_MacroMapPath;
        Ref<Texture2D> m_MacroMap;

        // GPU texture arrays (one layer per array slice), possibly shared with
        // other materials built from the same layers.
        Ref<TerrainLayerArrays> m_LayerArrays;

        u32 m_LayerResolution = kDefaultLayerResolution; // Resolution of each layer in the texture arrays

        // CPU-side splatmap pixel buffers (RGBA8, row-major). A MIRROR of the GPU
        // splatmaps while painting; mutable so SyncSplatmapsFromGPU() can refresh
        // it from the const read accessor.
        mutable std::array<TArray<u8>, 2> m_CPUSplatmaps;
        mutable bool m_CPUSplatmapsStale = false;
        u64 m_SplatmapRevision = 0;
        u32 m_SplatmapResolution = 0;
    };

} // namespace OloEngine

#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace OloEngine
{
    // One scene entity's region in the baked lightmap atlas: the entity's
    // second UV set (uv2) is mapped into the atlas as `uv2 * ScaleOffset.xy +
    // ScaleOffset.zw` on the given page. Written verbatim into the `.olmap`
    // EntityTable section, so the layout is on-disk ABI — see the
    // static_asserts below and Serialization/LightmapBinaryFormat.h.
    struct LightmapEntityEntry
    {
        u64 EntityUUID = 0;            // scene entity this region belongs to
        u32 Page = 0;                  // atlas page index (v1: always 0)
        u32 _pad0 = 0;                 // explicit padding — keeps the wire layout deterministic
        glm::vec4 ScaleOffset{ 0.0f }; // uv2 * xy + zw addresses the entity's charts in the atlas
    };

    static_assert(std::is_trivially_copyable_v<LightmapEntityEntry>);
    static_assert(std::is_standard_layout_v<LightmapEntityEntry>);
    static_assert(sizeof(LightmapEntityEntry) == 32);
    static_assert(offsetof(LightmapEntityEntry, EntityUUID) == 0);
    static_assert(offsetof(LightmapEntityEntry, Page) == 8);
    static_assert(offsetof(LightmapEntityEntry, _pad0) == 12);
    static_assert(offsetof(LightmapEntityEntry, ScaleOffset) == 16);

    // Baked GI lightmap atlas (issue #439).
    //
    // Texels store INDIRECT IRRADIANCE E in the reference path tracer's
    // physical units — the same storage convention as the DDGI irradiance
    // atlas ("the atlas stores full irradiance E"); see
    // docs/agent-rules/reference-path-tracer.md §4. The alpha channel is 1.0
    // for baked texels and 0.0 for texels no bake ever wrote, so samplers can
    // tell "black because unlit" from "black because never baked".
    //
    // Layout: PageCount square pages of Width×Height RGBA f32 texels, flat in
    // m_TexelData as page-major, row-major within a page
    // (page * Width * Height * 4 + (y * Width + x) * 4). Pages are expected
    // square power-of-two but that is not enforced here.
    class LightmapAsset : public Asset
    {
      public:
        LightmapAsset() = default;
        // Data-carrying constructor for EditorAssetManager::CreateOrReplaceAsset,
        // which constructs the stored instance in place — the bake's result moves
        // straight in, so the first Serialize already writes real data (the
        // serializer refuses an empty/invalid asset by design).
        LightmapAsset(u32 width, u32 height, u32 pageCount, u64 bakeKey,
                      std::vector<f32>&& texelData, std::vector<LightmapEntityEntry>&& entries)
            : m_Width(width), m_Height(height), m_PageCount(pageCount), m_BakeKey(bakeKey),
              m_TexelData(std::move(texelData)), m_Entries(std::move(entries))
        {
        }
        ~LightmapAsset() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::Lightmap;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        [[nodiscard]] u32 GetWidth() const noexcept
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const noexcept
        {
            return m_Height;
        }
        [[nodiscard]] u32 GetPageCount() const noexcept
        {
            return m_PageCount;
        }
        void SetDimensions(u32 width, u32 height, u32 pageCount = 1) noexcept
        {
            m_Width = width;
            m_Height = height;
            m_PageCount = pageCount;
        }

        // FNV-1a hash of the baked scene state; 0 = unset. Lets the runtime
        // detect that the scene drifted from what this atlas was baked
        // against without re-reading the whole scene.
        [[nodiscard]] u64 GetBakeKey() const noexcept
        {
            return m_BakeKey;
        }
        void SetBakeKey(u64 bakeKey) noexcept
        {
            m_BakeKey = bakeKey;
        }

        [[nodiscard]] const std::vector<f32>& GetTexelData() const noexcept
        {
            return m_TexelData;
        }
        [[nodiscard]] std::vector<f32>& GetTexelData() noexcept
        {
            return m_TexelData;
        }
        void SetTexelData(std::vector<f32>&& texelData)
        {
            m_TexelData = std::move(texelData);
        }

        [[nodiscard]] const std::vector<LightmapEntityEntry>& GetEntries() const noexcept
        {
            return m_Entries;
        }
        [[nodiscard]] std::vector<LightmapEntityEntry>& GetEntries() noexcept
        {
            return m_Entries;
        }
        void SetEntries(std::vector<LightmapEntityEntry>&& entries)
        {
            m_Entries = std::move(entries);
        }

        // Number of f32s a consistent texel buffer must hold:
        // PageCount * Width * Height * 4 (RGBA).
        [[nodiscard]] u64 GetExpectedTexelCount() const noexcept
        {
            return static_cast<u64>(m_PageCount) * m_Width * m_Height * 4u;
        }

        [[nodiscard]] bool HasBakedData() const noexcept
        {
            return !m_TexelData.empty();
        }

        // Resize the texel buffer to the expected count, zero-filled (alpha 0
        // == "never written"). No-op when the dimensions are unset.
        void AllocateTexels()
        {
            u64 const count = GetExpectedTexelCount();
            if (count == 0)
            {
                return;
            }
            m_TexelData.assign(static_cast<sizet>(count), 0.0f);
        }

        // Internal-consistency check the loader (and the writer) rely on:
        //  - dimensions are set (Width, Height, PageCount all non-zero),
        //  - the texel buffer holds exactly PageCount*Width*Height*4 floats,
        //  - every texel is finite (no NaN/Inf may reach the renderer),
        //  - every entity entry addresses an existing page with a finite
        //    scale/offset.
        // Deliberately does NOT enforce the on-disk format's size caps —
        // those live in Serialization/LightmapBinaryFormat.h and belong to
        // the serializer.
        [[nodiscard]] bool Validate() const
        {
            if (m_Width == 0 || m_Height == 0 || m_PageCount == 0)
            {
                return false;
            }
            if (static_cast<u64>(m_TexelData.size()) != GetExpectedTexelCount())
            {
                return false;
            }
            for (f32 const texel : m_TexelData)
            {
                if (!std::isfinite(texel))
                {
                    return false;
                }
            }
            for (const auto& entry : m_Entries)
            {
                if (entry.Page >= m_PageCount)
                {
                    return false;
                }
                for (glm::length_t c = 0; c < 4; ++c)
                {
                    if (!std::isfinite(entry.ScaleOffset[c]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

      private:
        u32 m_Width = 0;     // atlas width in texels (square, pow2 expected but not enforced here)
        u32 m_Height = 0;    // atlas height in texels
        u32 m_PageCount = 1; // number of atlas pages (v1 bakes always produce 1)
        u64 m_BakeKey = 0;   // FNV-1a hash of the baked scene state; 0 = unset

        // Raw RGBA f32, PageCount*Width*Height*4 floats, linear irradiance E.
        std::vector<f32> m_TexelData;
        std::vector<LightmapEntityEntry> m_Entries;
    };
} // namespace OloEngine

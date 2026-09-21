#pragma once

#include "OloEngine/Core/Base.h"

#include <utility>

namespace OloEngine
{
    enum class ViewLayerType : u8
    {
        ThreeD = 0, // 3D geometry
        TwoD,       // 2D sprites/UI elements
        UI,         // UI overlays
        Skybox,     // Skybox rendering
        Highest = Skybox + 1
    };

    enum class RenderMode : u8
    {
        Opaque = 0,  // Opaque geometry (front-to-back)
        Transparent, // Conventional source-over alpha (back-to-front)
        Additive,    // Additive blending
        Subtractive  // Subtractive blending
    };

    /// @brief Sorting key for render commands to minimize state changes and optimize rendering order.
    /// Uses a packed 64-bit integer for fast comparison and sorting.
    ///
    /// The payload layout depends on the RenderMode — see the bit-layout comment
    /// in the private section. Construct through the factories; the mutators are
    /// layout-aware and may be called in any order.
    class DrawKey
    {
      public:
        DrawKey() = default;
        explicit DrawKey(u64 key) : m_Key(key) {}

        // Primary construction methods
        static DrawKey CreateOpaque(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth);
        static DrawKey CreateTransparent(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth);
        static DrawKey CreateCustom(u32 viewportID, ViewLayerType viewLayer, u32 priority);

        // Accessors
        [[nodiscard("Store this!")]] u32 GetViewportID() const;
        [[nodiscard("Store this!")]] ViewLayerType GetViewLayer() const;
        [[nodiscard("Store this!")]] RenderMode GetRenderMode() const;
        [[nodiscard("Store this!")]] u32 GetShaderID() const;
        [[nodiscard("Store this!")]] u32 GetMaterialID() const;
        [[nodiscard("Store this!")]] u32 GetDepth() const;
        [[nodiscard("Store this!")]] u32 GetPriority() const;

        /// True when this key's payload is ordered depth-first (conventional
        /// alpha). False for the state-major modes.
        [[nodiscard("Store this!")]] bool IsDepthMajor() const;
        [[nodiscard]] static constexpr bool IsDepthMajor(RenderMode mode)
        {
            // Only conventional source-over alpha needs a global back-to-front
            // order. Additive and subtractive accumulation commute, so they
            // keep the state-major layout and its cheaper state changes.
            return mode == RenderMode::Transparent;
        }

        // Mutators
        void SetViewportID(u32 viewportID);
        void SetViewLayer(ViewLayerType viewLayer);
        void SetRenderMode(RenderMode mode);
        void SetShaderID(u32 shaderID);
        void SetMaterialID(u32 materialID);
        void SetDepth(u32 depth);
        void SetPriority(u32 priority);

        // Comparison operators for sorting
        // Ascending raw key order — matches the LSB radix sort used by CommandBucket.
        // Lower ViewportID/ViewLayer/RenderMode values are dispatched first.
        bool operator<(const DrawKey& other) const
        {
            return m_Key < other.m_Key;
        }
        bool operator==(const DrawKey& other) const
        {
            return m_Key == other.m_Key;
        }
        bool operator!=(const DrawKey& other) const
        {
            return m_Key != other.m_Key;
        } // Get raw key value
        [[nodiscard("Store this!")]] u64 GetKey() const
        {
            return m_Key;
        }
        void SetKey(u64 key)
        {
            m_Key = key;
        }

        explicit operator u64() const
        {
            return m_Key;
        }

      private:
        // Bit layout for the 64-bit key. The three high fields are shared by
        // every render mode; the low 56 bits are laid out PER MODE.
        //
        //   [63:61] ViewportID (3 bits)
        //   [60:58] ViewLayer  (3 bits)
        //   [57:56] RenderMode (2 bits)
        //
        // Opaque / Additive / Subtractive — state-major, to minimise binds:
        //   [55:40] ShaderID   (16 bits)
        //   [39:24] MaterialID (16 bits)
        //   [23:0]  Depth      (24 bits; near first, i.e. front-to-back)
        //
        // Transparent — DEPTH-MAJOR (issue #1327):
        //   [55:32] Depth      (24 bits; stored inverted, i.e. back-to-front)
        //   [31:16] ShaderID   (16 bits)
        //   [15:0]  MaterialID (16 bits)
        //
        // Why two layouts instead of two sorts: RenderMode sits ABOVE the
        // payload, so opaque and transparent keys can never interleave in a raw
        // 64-bit comparison. Each partition is therefore free to order its own
        // payload, and the field-agnostic LSB radix sort in CommandBucket
        // produces both orders in the same eight passes. Depth used to be the
        // LEAST significant field for transparents too, which ordered them only
        // within one shader+material bucket: two overlapping 50% surfaces with
        // different materials blended in material-ID order.
        //
        // Tie-breaking, for transparents, is therefore depth, then shader, then
        // material, then submission order (the radix sort is stable).
        //
        // LIMITATION — object-level ordering only. The depth is one quantised
        // value per DRAW, taken from the object's origin or bounding-sphere
        // centre in view space (Renderer3DDrawHelpers::ComputeDepthForSortKey).
        // Two transparent meshes that INTERSECT blend in whole-object order at
        // every pixel, including the pixels where the other mesh is in front.
        // Resolving that per fragment requires the weighted-blended OIT path
        // (decals, particles and groom route through it under
        // RendererSettings::OITEnabled); sorting draws cannot and does not fix
        // it. See docs/agent-rules/transparent-draw-order.md.

        static constexpr u64 VIEWPORT_SHIFT = 61;
        static constexpr u64 VIEWPORT_MASK = 0x7ULL;

        static constexpr u64 VIEWLAYER_SHIFT = 58;
        static constexpr u64 VIEWLAYER_MASK = 0x7ULL;

        static constexpr u64 RENDERMODE_SHIFT = 56;
        static constexpr u64 RENDERMODE_MASK = 0x3ULL;

        // Everything below the RenderMode bits: the mode-dependent payload.
        static constexpr u64 PAYLOAD_MASK = (1ULL << RENDERMODE_SHIFT) - 1ULL;

        static constexpr u64 SHADER_MASK = 0xFFFFULL;
        static constexpr u64 MATERIAL_MASK = 0xFFFFULL;
        static constexpr u64 DEPTH_MASK = 0xFFFFFFULL;

        // State-major payload (Opaque / Additive / Subtractive).
        static constexpr u64 SHADER_SHIFT = 40;
        static constexpr u64 MATERIAL_SHIFT = 24;
        static constexpr u64 DEPTH_SHIFT = 0;

        // Depth-major payload (Transparent).
        static constexpr u64 BLEND_DEPTH_SHIFT = 32;
        static constexpr u64 BLEND_SHADER_SHIFT = 16;
        static constexpr u64 BLEND_MATERIAL_SHIFT = 0;

        [[nodiscard]] static constexpr u64 ShaderShiftFor(RenderMode mode)
        {
            return IsDepthMajor(mode) ? BLEND_SHADER_SHIFT : SHADER_SHIFT;
        }
        [[nodiscard]] static constexpr u64 MaterialShiftFor(RenderMode mode)
        {
            return IsDepthMajor(mode) ? BLEND_MATERIAL_SHIFT : MATERIAL_SHIFT;
        }
        [[nodiscard]] static constexpr u64 DepthShiftFor(RenderMode mode)
        {
            return IsDepthMajor(mode) ? BLEND_DEPTH_SHIFT : DEPTH_SHIFT;
        }

        // Compose a whole key in one pass. The public mutators are
        // read-modify-write and SetRenderMode re-packs the payload; none of
        // that is needed when the key is being built from nothing, and this
        // runs on every draw submission.
        [[nodiscard]] static u64 Pack(u32 viewportID, ViewLayerType viewLayer, RenderMode mode,
                                      u32 shaderID, u32 materialID, u32 depth);

        u64 m_Key = 0;
    };

    // Inline implementations
    inline u64 DrawKey::Pack(u32 viewportID, ViewLayerType viewLayer, RenderMode mode,
                             u32 shaderID, u32 materialID, u32 depth)
    {
        const u32 layer = static_cast<u32>(std::to_underlying(viewLayer));
        const u32 modeValue = static_cast<u32>(std::to_underlying(mode));
        OLO_CORE_ASSERT(viewportID <= VIEWPORT_MASK, "ViewportID too large");
        OLO_CORE_ASSERT(layer <= VIEWLAYER_MASK, "ViewLayer value too large");
        OLO_CORE_ASSERT(modeValue <= RENDERMODE_MASK, "RenderMode value too large");
        OLO_CORE_ASSERT(shaderID <= SHADER_MASK, "ShaderID too large");
        OLO_CORE_ASSERT(materialID <= MATERIAL_MASK, "MaterialID too large");
        OLO_CORE_ASSERT(depth <= DEPTH_MASK, "Depth value too large");

        return (static_cast<u64>(viewportID) << VIEWPORT_SHIFT) |
               (static_cast<u64>(layer) << VIEWLAYER_SHIFT) |
               (static_cast<u64>(modeValue) << RENDERMODE_SHIFT) |
               (static_cast<u64>(shaderID) << ShaderShiftFor(mode)) |
               (static_cast<u64>(materialID) << MaterialShiftFor(mode)) |
               (static_cast<u64>(depth) << DepthShiftFor(mode));
    }

    inline DrawKey DrawKey::CreateOpaque(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth)
    {
        DrawKey key;
        key.m_Key = Pack(viewportID, viewLayer, RenderMode::Opaque, shaderID, materialID, depth);
        return key;
    }

    inline DrawKey DrawKey::CreateTransparent(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth)
    {
        OLO_CORE_ASSERT(depth <= DEPTH_MASK, "Depth value too large");
        DrawKey key;
        // Invert depth so back-to-front falls out of the same ascending raw-key
        // sort the opaque partition uses. The inverted value is the stored
        // field, which is what GetDepth() reports.
        key.m_Key = Pack(viewportID, viewLayer, RenderMode::Transparent, shaderID, materialID,
                         static_cast<u32>(DEPTH_MASK) - depth);
        return key;
    }

    inline DrawKey DrawKey::CreateCustom(u32 viewportID, ViewLayerType viewLayer, u32 priority)
    {
        DrawKey key;
        // Custom commands use opaque mode; priority occupies the depth field.
        key.m_Key = Pack(viewportID, viewLayer, RenderMode::Opaque, 0, 0, priority);
        return key;
    }

    inline u32 DrawKey::GetViewportID() const
    {
        return static_cast<u32>((m_Key >> VIEWPORT_SHIFT) & VIEWPORT_MASK);
    }

    inline ViewLayerType DrawKey::GetViewLayer() const
    {
        return static_cast<ViewLayerType>((m_Key >> VIEWLAYER_SHIFT) & VIEWLAYER_MASK);
    }

    inline RenderMode DrawKey::GetRenderMode() const
    {
        return static_cast<RenderMode>((m_Key >> RENDERMODE_SHIFT) & RENDERMODE_MASK);
    }

    inline bool DrawKey::IsDepthMajor() const
    {
        return IsDepthMajor(GetRenderMode());
    }

    inline u32 DrawKey::GetShaderID() const
    {
        return static_cast<u32>((m_Key >> ShaderShiftFor(GetRenderMode())) & SHADER_MASK);
    }

    inline u32 DrawKey::GetMaterialID() const
    {
        return static_cast<u32>((m_Key >> MaterialShiftFor(GetRenderMode())) & MATERIAL_MASK);
    }

    inline u32 DrawKey::GetDepth() const
    {
        return static_cast<u32>((m_Key >> DepthShiftFor(GetRenderMode())) & DEPTH_MASK);
    }

    inline u32 DrawKey::GetPriority() const
    {
        return GetDepth(); // Priority uses the same bits as depth
    }

    inline void DrawKey::SetViewportID(u32 viewportID)
    {
        OLO_CORE_ASSERT(viewportID <= VIEWPORT_MASK, "ViewportID too large");
        m_Key = (m_Key & ~(VIEWPORT_MASK << VIEWPORT_SHIFT)) | (static_cast<u64>(viewportID) << VIEWPORT_SHIFT);
    }

    inline void DrawKey::SetViewLayer(ViewLayerType viewLayer)
    {
        u32 layer = static_cast<u32>(std::to_underlying(viewLayer));
        OLO_CORE_ASSERT(layer <= VIEWLAYER_MASK, "ViewLayer value too large");
        m_Key = (m_Key & ~(VIEWLAYER_MASK << VIEWLAYER_SHIFT)) | (static_cast<u64>(layer) << VIEWLAYER_SHIFT);
    }

    inline void DrawKey::SetRenderMode(RenderMode mode)
    {
        u32 modeValue = static_cast<u32>(std::to_underlying(mode));
        OLO_CORE_ASSERT(modeValue <= RENDERMODE_MASK, "RenderMode value too large");
        // The payload layout depends on the mode, so the three fields are read
        // out under the OLD interpretation and written back under the new one.
        // Without this, setting the mode after the fields would silently
        // reinterpret 56 bits as a different set of fields, and the mutators
        // would only be order-independent by accident.
        const u32 shaderID = GetShaderID();
        const u32 materialID = GetMaterialID();
        const u32 depth = GetDepth();
        m_Key = (m_Key & ~((RENDERMODE_MASK << RENDERMODE_SHIFT) | PAYLOAD_MASK)) |
                (static_cast<u64>(modeValue) << RENDERMODE_SHIFT);
        SetShaderID(shaderID);
        SetMaterialID(materialID);
        SetDepth(depth);
    }

    inline void DrawKey::SetShaderID(u32 shaderID)
    {
        OLO_CORE_ASSERT(shaderID <= SHADER_MASK, "ShaderID too large");
        const u64 shift = ShaderShiftFor(GetRenderMode());
        m_Key = (m_Key & ~(SHADER_MASK << shift)) | (static_cast<u64>(shaderID) << shift);
    }

    inline void DrawKey::SetMaterialID(u32 materialID)
    {
        OLO_CORE_ASSERT(materialID <= MATERIAL_MASK, "MaterialID too large");
        const u64 shift = MaterialShiftFor(GetRenderMode());
        m_Key = (m_Key & ~(MATERIAL_MASK << shift)) | (static_cast<u64>(materialID) << shift);
    }

    inline void DrawKey::SetDepth(u32 depth)
    {
        OLO_CORE_ASSERT(depth <= DEPTH_MASK, "Depth value too large");
        const u64 shift = DepthShiftFor(GetRenderMode());
        m_Key = (m_Key & ~(DEPTH_MASK << shift)) | (static_cast<u64>(depth) << shift);
    }

    inline void DrawKey::SetPriority(u32 priority)
    {
        SetDepth(priority); // Priority uses the same bits as depth
    }

    // Utility functions for debugging
    inline const char* ToString(ViewLayerType type)
    {
        switch (type)
        {
            case ViewLayerType::ThreeD:
                return "3D";
            case ViewLayerType::TwoD:
                return "2D";
            case ViewLayerType::UI:
                return "UI";
            case ViewLayerType::Skybox:
                return "Skybox";
            default:
                OLO_CORE_ASSERT(false, "Unknown ViewLayerType");
                return "Unknown";
        }
    }

    inline const char* ToString(RenderMode mode)
    {
        switch (mode)
        {
            case RenderMode::Opaque:
                return "Opaque";
            case RenderMode::Transparent:
                return "Transparent";
            case RenderMode::Additive:
                return "Additive";
            case RenderMode::Subtractive:
                return "Subtractive";
            default:
                OLO_CORE_ASSERT(false, "Unknown RenderMode");
                return "Unknown";
        }
    }

} // namespace OloEngine

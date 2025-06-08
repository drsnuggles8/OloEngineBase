#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    enum class ViewLayerType : u8
    {
        ThreeD = 0,         // 3D geometry
        TwoD,               // 2D sprites/UI elements  
        UI,                 // UI overlays
        Skybox,             // Skybox rendering
        Highest = Skybox + 1
    };

    enum class RenderMode : u8
    {
        Opaque = 0,         // Opaque geometry (front-to-back)
        Transparent,        // Transparent geometry (back-to-front)
        Additive,           // Additive blending
        Subtractive         // Subtractive blending
    };

    /// @brief Sorting key for render commands to minimize state changes and optimize rendering order.
    /// Uses a packed 64-bit integer for fast comparison and sorting.
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
        u32 GetViewportID() const;
        ViewLayerType GetViewLayer() const;
        RenderMode GetRenderMode() const;
        u32 GetShaderID() const;
        u32 GetMaterialID() const;
        u32 GetDepth() const;
        u32 GetPriority() const;

        // Mutators
        void SetViewportID(u32 viewportID);
        void SetViewLayer(ViewLayerType viewLayer);
        void SetRenderMode(RenderMode mode);
        void SetShaderID(u32 shaderID);
        void SetMaterialID(u32 materialID);
        void SetDepth(u32 depth);
        void SetPriority(u32 priority);

        // Comparison operators for sorting
        bool operator<(const DrawKey& other) const { return m_Key > other.m_Key; } // Higher values = higher priority
        bool operator==(const DrawKey& other) const { return m_Key == other.m_Key; }
        bool operator!=(const DrawKey& other) const { return m_Key != other.m_Key; }

        // Get raw key value
        u64 GetKey() const { return m_Key; }
        void SetKey(u64 key) { m_Key = key; }

        explicit operator u64() const { return m_Key; }

    private:
        // Bit layout for 64-bit key:
        // [63:61] ViewportID (3 bits)
        // [60:58] ViewLayer (3 bits)  
        // [57:56] RenderMode (2 bits)
        // [55:40] ShaderID (16 bits)
        // [39:24] MaterialID (16 bits)
        // [23:0]  Depth/Priority (24 bits)

        static constexpr u64 VIEWPORT_SHIFT = 61;
        static constexpr u64 VIEWPORT_MASK = 0x7ULL;
        
        static constexpr u64 VIEWLAYER_SHIFT = 58;
        static constexpr u64 VIEWLAYER_MASK = 0x7ULL;
        
        static constexpr u64 RENDERMODE_SHIFT = 56;
        static constexpr u64 RENDERMODE_MASK = 0x3ULL;
        
        static constexpr u64 SHADER_SHIFT = 40;
        static constexpr u64 SHADER_MASK = 0xFFFFULL;
        
        static constexpr u64 MATERIAL_SHIFT = 24;
        static constexpr u64 MATERIAL_MASK = 0xFFFFULL;
        
        static constexpr u64 DEPTH_SHIFT = 0;
        static constexpr u64 DEPTH_MASK = 0xFFFFFFULL;

        u64 m_Key = 0;
    };

    // Inline implementations
    inline DrawKey DrawKey::CreateOpaque(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth)
    {
        DrawKey key;
        key.SetViewportID(viewportID);
        key.SetViewLayer(viewLayer);
        key.SetRenderMode(RenderMode::Opaque);
        key.SetShaderID(shaderID);
        key.SetMaterialID(materialID);
        key.SetDepth(depth);
        return key;
    }

    inline DrawKey DrawKey::CreateTransparent(u32 viewportID, ViewLayerType viewLayer, u32 shaderID, u32 materialID, u32 depth)
    {
        DrawKey key;
        key.SetViewportID(viewportID);
        key.SetViewLayer(viewLayer);
        key.SetRenderMode(RenderMode::Transparent);
        key.SetShaderID(shaderID);
        key.SetMaterialID(materialID);
        key.SetDepth(0xFFFFFF - depth); // Invert depth for back-to-front sorting
        return key;
    }

    inline DrawKey DrawKey::CreateCustom(u32 viewportID, ViewLayerType viewLayer, u32 priority)
    {
        DrawKey key;
        key.SetViewportID(viewportID);
        key.SetViewLayer(viewLayer);
        key.SetRenderMode(RenderMode::Opaque); // Custom commands use opaque mode
        key.SetPriority(priority);
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

    inline u32 DrawKey::GetShaderID() const
    {
        return static_cast<u32>((m_Key >> SHADER_SHIFT) & SHADER_MASK);
    }

    inline u32 DrawKey::GetMaterialID() const
    {
        return static_cast<u32>((m_Key >> MATERIAL_SHIFT) & MATERIAL_MASK);
    }

    inline u32 DrawKey::GetDepth() const
    {
        return static_cast<u32>((m_Key >> DEPTH_SHIFT) & DEPTH_MASK);
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
        u32 layer = static_cast<u32>(viewLayer);
        OLO_CORE_ASSERT(layer <= VIEWLAYER_MASK, "ViewLayer value too large");
        m_Key = (m_Key & ~(VIEWLAYER_MASK << VIEWLAYER_SHIFT)) | (static_cast<u64>(layer) << VIEWLAYER_SHIFT);
    }

    inline void DrawKey::SetRenderMode(RenderMode mode)
    {
        u32 modeValue = static_cast<u32>(mode);
        OLO_CORE_ASSERT(modeValue <= RENDERMODE_MASK, "RenderMode value too large");
        m_Key = (m_Key & ~(RENDERMODE_MASK << RENDERMODE_SHIFT)) | (static_cast<u64>(modeValue) << RENDERMODE_SHIFT);
    }

    inline void DrawKey::SetShaderID(u32 shaderID)
    {
        OLO_CORE_ASSERT(shaderID <= SHADER_MASK, "ShaderID too large");
        m_Key = (m_Key & ~(SHADER_MASK << SHADER_SHIFT)) | (static_cast<u64>(shaderID) << SHADER_SHIFT);
    }

    inline void DrawKey::SetMaterialID(u32 materialID)
    {
        OLO_CORE_ASSERT(materialID <= MATERIAL_MASK, "MaterialID too large");
        m_Key = (m_Key & ~(MATERIAL_MASK << MATERIAL_SHIFT)) | (static_cast<u64>(materialID) << MATERIAL_SHIFT);
    }

    inline void DrawKey::SetDepth(u32 depth)
    {
        OLO_CORE_ASSERT(depth <= DEPTH_MASK, "Depth value too large");
        m_Key = (m_Key & ~(DEPTH_MASK << DEPTH_SHIFT)) | (static_cast<u64>(depth) << DEPTH_SHIFT);
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
            case ViewLayerType::ThreeD: return "3D";
            case ViewLayerType::TwoD: return "2D";
            case ViewLayerType::UI: return "UI";
            case ViewLayerType::Skybox: return "Skybox";
            default:
                OLO_CORE_ASSERT(false, "Unknown ViewLayerType");
                return "Unknown";
        }
    }

    inline const char* ToString(RenderMode mode)
    {
        switch (mode)
        {
            case RenderMode::Opaque: return "Opaque";
            case RenderMode::Transparent: return "Transparent";
            case RenderMode::Additive: return "Additive";
            case RenderMode::Subtractive: return "Subtractive";
            default:
                OLO_CORE_ASSERT(false, "Unknown RenderMode");
                return "Unknown";
        }
    }

} // namespace OloEngine

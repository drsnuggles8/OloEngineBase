#pragma once

#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{

    /**
     * @brief Base class for all renderer resources that are also assets
     * 
     * This class provides the bridge between the asset management system
     * and renderer resources like textures, shaders, materials, etc.
     * Similar to Hazel's architecture.
     */
    class RendererResource : public Asset
    {
    public:
        RendererResource() = default;
        virtual ~RendererResource() = default;

        static AssetType GetStaticType() { return AssetType::None; }
        virtual AssetType GetAssetType() const override { return AssetType::None; }
    };

}

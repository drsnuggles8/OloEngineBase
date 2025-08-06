#pragma once

#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{
    using ResourceDescriptorInfo = void*;

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

        /**
         * @brief Get descriptor information for this resource
         * @return Descriptor info for binding to graphics APIs
         */
        virtual ResourceDescriptorInfo GetDescriptorInfo() const = 0;

        static AssetType GetStaticType() { return AssetType::None; }
        virtual AssetType GetAssetType() const override { return AssetType::None; }
    };

}

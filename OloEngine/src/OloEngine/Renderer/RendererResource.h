#pragma once

#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{

    // @brief Base class for all renderer resources that are also assets
    //
    // This class provides the bridge between the asset management system
    // and renderer resources like textures, shaders, materials, etc.
    class RendererResource : public Asset
    {
      public:
        virtual ~RendererResource() override = default;

        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::None;
        }
        virtual AssetType GetAssetType() const override = 0;

      protected:
        RendererResource() = default;
    };
} // namespace OloEngine

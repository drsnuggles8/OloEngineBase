#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"

#include <string>
#include <vector>

namespace OloEngine
{
    struct CubemapSpecification
    {
        u32 Width = 1;
        u32 Height = 1;
        ImageFormat Format = ImageFormat::RGBA8;
        bool GenerateMips = true;
    };

    class TextureCubemap : public Texture
    {
      public:
        virtual ~TextureCubemap() = default;

        // Create from 6 individual face images
        static Ref<TextureCubemap> Create(const std::vector<std::string>& facePaths);

        // Create empty cubemap with specification
        static Ref<TextureCubemap> Create(const CubemapSpecification& specification);

        // Set data for a specific face
        virtual void SetFaceData(u32 faceIndex, void* data, u32 size) = 0;

        virtual const CubemapSpecification& GetCubemapSpecification() const = 0;

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::TextureCube;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine

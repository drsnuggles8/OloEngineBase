#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Particle/ParticleSystem.h"

namespace OloEngine
{
    class ParticleSystemAsset : public Asset
    {
    public:
        ParticleSystemAsset() = default;

        static AssetType GetStaticType() { return AssetType::ParticleSystem; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }

        ParticleSystem System;
    };
}

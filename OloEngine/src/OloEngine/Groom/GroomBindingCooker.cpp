#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomBindingCooker.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <format>
#include <utility>

namespace OloEngine
{
    namespace GroomBindingCooker
    {
        bool CookToBytes(const GroomBindingAsset& binding, std::vector<u8>& outBytes, std::string& outReason)
        {
            // Delegated rather than reimplemented: the serializer's encode IS
            // the format, and a second writer beside it would be a second thing
            // that can disagree with the reader.
            return GroomBindingSerializer::EncodeToBytes(binding, outBytes, outReason);
        }

        bool CookPair(const GroomAsset& groom, const GroomSurfaceView& target, const std::string& targetSourcePath,
                      const GroomBindingBuildSettings& settings, std::vector<u8>& outBytes,
                      Ref<GroomBindingAsset>& outBinding, GroomBindingBuildStats& outStats, std::string& outReason)
        {
            Ref<GroomBindingAsset> binding;
            if (!GroomBindingBuilder::Build(groom, target, targetSourcePath, settings, binding, outStats, outReason))
            {
                return false;
            }

            std::vector<u8> bytes;
            if (!CookToBytes(*binding, bytes, outReason))
            {
                return false;
            }

            // Assigned only after every step has succeeded — the all-or-nothing
            // rule the serializer states for a decode, applied to a cook.
            outBytes = std::move(bytes);
            outBinding = binding;
            return true;
        }
    } // namespace GroomBindingCooker
} // namespace OloEngine

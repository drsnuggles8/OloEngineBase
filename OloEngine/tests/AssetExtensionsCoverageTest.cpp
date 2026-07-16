// =============================================================================
// AssetExtensionsCoverageTest.cpp
//
// Meta-test: every `AssetType::*` enum value (except `None` and
// known runtime-only types) has at least one file extension registered
// in `AssetExtensions::s_ExtensionMap`. Without a registered extension,
// the editor's asset-import pipeline can't auto-detect the asset type
// when a developer drops a file in — the file ends up as `AssetType::None`
// and silently fails to import.
//
// Detection
// ---------
//   Iterate every value of `AssetType` from 1 to the highest declared.
//   For each, query `AssetExtensions::GetExtensionsForAssetType(t)` and
//   assert the returned vector is non-empty.
//
// Known exceptions
// ----------------
//   Some enum values are "logical" types with no on-disk file
//   representation by design — e.g., `AssetType::Script` is the
//   abstract concept of a script (engine-loaded), distinct from
//   `AssetType::ScriptFile` which is the `.cs` source.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/AssetExtensions.h"

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    TEST(AssetExtensionsCoverage, EveryAssetTypeHasAtLeastOneRegisteredExtension)
    {
        // AssetType values that legitimately have no on-disk extension:
        const std::set<AssetType> kRuntimeOnly = {
            AssetType::None,
            // Logical wrappers — backed by other asset types on disk:
            AssetType::Script,               // abstract C# script (file is `ScriptFile` / .cs)
            AssetType::Shader,               // compiled at runtime from .glsl; no asset entry
            AssetType::ComputeShader,        // ditto
            AssetType::Model,                // runtime container — uses `MeshSource` (.fbx/.gltf/...)
            AssetType::Environment,          // runtime env map — uses `EnvMap` source
            AssetType::SpatializationConfig, // audio runtime config; no asset file yet
            AssetType::Terrain,              // procedural — generated, not imported
            AssetType::NavMesh,              // baked at runtime from scene geometry
            AssetType::SoundGraphSound,      // runtime audio source instance — built from a `SoundGraph` (.olosoundgraph) asset; no on-disk format of its own
            AssetType::TextureCube,          // built at runtime from 6 .png faces (or an HDR equirect → cubemap conversion); no dedicated extension
        };

        // Walk every declared enum value via static_cast loop. The
        // current top is `CharacterClassDatabase = 38`; if more get added,
        // bumping this is the only maintenance.
        constexpr u16 kMaxKnownValue = 38;

        std::vector<AssetType> uncovered;
        for (u16 raw = 1; raw <= kMaxKnownValue; ++raw)
        {
            const AssetType t = static_cast<AssetType>(raw);
            if (kRuntimeOnly.contains(t))
                continue;
            const auto extensions = AssetExtensions::GetExtensionsForAssetType(t);
            if (extensions.empty())
                uncovered.push_back(t);
        }

        if (!uncovered.empty())
        {
            std::ostringstream oss;
            oss << uncovered.size()
                << " AssetType value(s) have no registered extension in s_ExtensionMap:\n";
            for (auto t : uncovered)
                oss << "  - AssetType(" << std::to_underlying(t) << ")\n";
            oss << "\nEither register a file extension for the type in "
                << "`AssetExtensions::InitializeExtensionMap`, or — if the type is "
                << "intentionally runtime-only / has no on-disk format — append it "
                << "to the `kRuntimeOnly` exclusion set in this test with a "
                << "comment explaining why.\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // Every registered extension maps back to its own type symmetrically.
    //
    // Catches "I registered `tga` as `Texture2D` but also as `EnvMap`
    // elsewhere" — the map is supposed to be one-extension-to-one-type,
    // so `GetExtensionsForAssetType(GetAssetTypeFromExtension(ext))`
    // must include `ext` again.
    // -------------------------------------------------------------------------
    TEST(AssetExtensionsCoverage, ExtensionMapIsRoundTripSymmetric)
    {
        const auto allExtensions = AssetExtensions::GetAllSupportedExtensions();
        ASSERT_FALSE(allExtensions.empty());

        std::vector<std::string> brokenRoundTrip;
        for (const auto& ext : allExtensions)
        {
            const AssetType resolved = AssetExtensions::GetAssetTypeFromExtension(ext);
            if (resolved == AssetType::None)
            {
                brokenRoundTrip.push_back(
                    ext + " → AssetType::None (extension in supported list but "
                          "GetAssetTypeFromExtension returned None)");
                continue;
            }

            const auto returnExtensions = AssetExtensions::GetExtensionsForAssetType(resolved);
            bool foundInReverse = false;
            for (const auto& e : returnExtensions)
            {
                // Normalise: GetExtensionsForAssetType may return with
                // or without a leading dot. Compare without dots.
                std::string normalised = e;
                if (!normalised.empty() && normalised.front() == '.')
                    normalised.erase(0, 1);
                std::string lhs = ext;
                if (!lhs.empty() && lhs.front() == '.')
                    lhs.erase(0, 1);
                if (normalised == lhs)
                {
                    foundInReverse = true;
                    break;
                }
            }
            if (!foundInReverse)
            {
                brokenRoundTrip.push_back(
                    ext + " → AssetType(" + std::to_string(std::to_underlying(resolved)) +
                    ") but reverse lookup doesn't include this extension");
            }
        }

        if (!brokenRoundTrip.empty())
        {
            std::ostringstream oss;
            oss << brokenRoundTrip.size() << " asymmetric extension mapping(s):\n";
            for (const auto& s : brokenRoundTrip)
                oss << "  - " << s << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests

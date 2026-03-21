#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    /**
     * @brief Configuration for building a standalone game distribution
     *
     * These settings control how the Build Game pipeline assembles
     * a shippable game folder from the active project.
     */
    struct GameBuildSettings
    {
        /// Display name of the game (used for window title and output folder)
        std::string GameName = "MyGame";

        /// Root directory where the build output will be placed
        /// The final structure will be: OutputDirectory/GameName/
        std::filesystem::path OutputDirectory;

        /// Whether to compress assets in the pack file
        bool CompressAssets = true;

        /// Whether to include the C#/Lua script modules in the pack
        bool IncludeScriptModule = true;

        /// Whether to validate all assets before packing
        bool ValidateAssets = true;

        /// Build configuration to use for the runtime executable
        /// Values: "Debug", "Release", "Dist"
        std::string BuildConfiguration = "Release";

        /// Path to the start scene relative to the project asset directory.
        /// Example: "Scenes/GameplayAbilityTest.olo"
        /// If empty, the pipeline falls back to the project's configured StartScene.
        std::filesystem::path StartScene;

        /// Whether the game uses 3D rendering (true) or 2D-only (false).
        /// Controls which renderer the runtime initialises at startup.
        bool Is3DMode = true;
    };

} // namespace OloEngine

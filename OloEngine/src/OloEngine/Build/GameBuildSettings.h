#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <string>

namespace OloEngine
{
    /**
     * @brief Target platform a game build is produced for (#891)
     *
     * OloEngine has no cross-compilation toolchain — see
     * IsBuildTargetSupportedOnThisHost — so making the target explicit is
     * about intent, not capability: every host-specific pipeline step
     * (runtime executable name, Mono/C# scripting availability, icon vs.
     * .desktop handling) is selected deliberately instead of by accident of
     * whatever host happens to run the build.
     */
    enum class BuildTargetPlatform : u8
    {
        Windows,
        Linux,
    };

    /// Human-readable name, for log messages and the game manifest.
    const char* ToString(BuildTargetPlatform platform);

    /// The platform this process is running on, and the pipeline's default
    /// build target.
    BuildTargetPlatform GetHostBuildPlatform();

    /// Whether this host can actually produce a build for `platform`.
    /// Building copies the *host's own* runtime executable, Mono runtime and
    /// script assemblies — there is no cross-compilation toolchain, so this
    /// is only ever true when `platform == GetHostBuildPlatform()`.
    bool IsBuildTargetSupportedOnThisHost(BuildTargetPlatform platform);

    /// Filename convention for a native executable on `platform` — Windows
    /// appends `.exe`, Linux uses the bare name. Covers both looking up the
    /// prebuilt OloRuntime/OloEditor binaries and naming the packaged game's
    /// own executable.
    std::string GetHostExecutableFileName(const std::string& baseName, BuildTargetPlatform platform);

    /// Whether C# scripting can run on `platform`. OloEngine-ScriptCore only
    /// builds under the Visual Studio generator (see CLAUDE.md), so C#
    /// scripting is Windows-only regardless of what a build's other settings
    /// ask for — Lua scripting is unaffected.
    bool IsScriptingAvailableOnPlatform(BuildTargetPlatform platform);

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

        /// Optional path to a custom icon for the game executable. On Windows
        /// this must be a .ico file, embedded as a PE resource; if empty, the
        /// default OloEngine icon is used. On Linux this is written verbatim
        /// into the generated .desktop entry's `Icon=` field (#891), which
        /// expects a PNG/SVG/XPM file or icon-theme name — a Windows .ico here
        /// typically won't render, non-fatally falling back to a generic icon.
        std::filesystem::path IconPath;

        /// Default renderer backend the shipped game starts with (#691).
        /// Written to `config/renderer.yaml` next to the game executable —
        /// exactly the file the engine's backend selection reads, so the
        /// player's later `--rhi=` flag or an in-game settings write overrides
        /// it. Values: "opengl" (default), "vulkan".
        std::string DefaultRendererBackend = "opengl";

        /// Target platform this build is produced for (#891). Defaults to
        /// the host the editor is running on — the only target this host can
        /// currently build for (see IsBuildTargetSupportedOnThisHost).
        BuildTargetPlatform TargetPlatform = GetHostBuildPlatform();
    };

} // namespace OloEngine

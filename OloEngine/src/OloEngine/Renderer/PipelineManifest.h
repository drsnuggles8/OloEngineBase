#pragma once

// PipelineManifest: which complete pipeline keys each shader has needed, under
// which rendering configurations (issue #1345).
//
// WHY A RECORD AND NOT A DERIVATION. The renderer has no central
// (material, mesh kind, pass) -> shader function: the forward/G-Buffer choice
// is made at submit time, the depth-prepass swap at dispatch time, and every
// specialised pass (shadows, terrain, foliage, water, post) picks its own
// shader inside Execute(). A derivation that mirrored all of that would drift
// the first time a pass changed. The draw path, by contrast, sees the COMPLETE
// key every time it creates a pipeline. So the backend records each key it had
// to create, against the shader's identity and the configuration tag that was
// selected at the time, and a later session (or a packaged runtime that ships
// the manifest) requests those keys as soon as the shader exists, before its
// first draw.
//
// An entry is the part of the key that is NOT derived from the shader itself:
// the attachment formats, sample count and tessellation patch size. The shader
// identity is its name plus its stage mask, so a source edit that turns a
// classic shader into a mesh-stage one cannot inherit the other kind's entries.
// The root-data layout and the stage modules come from the live shader when the
// request is made, so an edited shader is requested against its NEW layout.
//
// Pure data: no backend headers, no file I/O beyond the string round trip.
// Format values are stored as integers; for Vulkan they are VkFormat values,
// which are fixed by the API registry.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OloEngine
{
    struct PipelineSignature
    {
        static constexpr u32 kMaxColorAttachments = 8;

        u32 ColorCount = 0;
        std::array<u32, kMaxColorAttachments> ColorFormats{};
        u32 DepthFormat = 0;
        u32 Samples = 1;
        u32 PatchControlPoints = 0; ///< 0 for a pipeline without tessellation stages.

        bool operator==(const PipelineSignature&) const = default;
    };

    struct PipelineManifestEntry
    {
        FString Shader;
        u32 StageMask = 0;
        PipelineSignature Signature;
        TArray<FString> Configurations; ///< Tags this key was drawn under, oldest first.
        u64 LastSeenSession = 0;

        [[nodiscard]] bool SeenUnder(std::string_view configuration) const;
    };

    template<>
    struct TIsTriviallyRelocatable<PipelineManifestEntry>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(PipelineManifestEntry::Shader)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineManifestEntry::StageMask)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineManifestEntry::Signature)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineManifestEntry::Configurations)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineManifestEntry::LastSeenSession)>;
    };

    class PipelineManifest
    {
      public:
        static constexpr u32 kFormatVersion = 1;
        // A manifest grows by one entry per distinct key a draw ever created.
        // Bounded so a long-lived cache directory shared by many projects
        // cannot make every launch request thousands of stale pipelines; the
        // least recently seen entries go first.
        static constexpr sizet kDefaultMaxEntries = 4096;
        static constexpr sizet kMaxConfigurationsPerEntry = 16;

        // A new session: entries recorded from now on are stamped with it.
        void BeginSession();
        [[nodiscard]] u64 GetSession() const
        {
            return m_Session;
        }

        // Record that `shader` needed `signature` while `configuration` was
        // selected. Returns true when the manifest changed (a new entry, or a
        // configuration this entry had not been seen under).
        bool Record(std::string_view shader, u32 stageMask, const PipelineSignature& signature,
                    std::string_view configuration);

        // Every entry for one shader identity, entries seen under `configuration`
        // first (the selected configuration is prepared before the others).
        [[nodiscard]] TArray<const PipelineManifestEntry*> EntriesFor(std::string_view shader, u32 stageMask,
                                                                      std::string_view configuration) const;

        // Add every entry of `other` (a shipped seed, or a second file); an
        // entry present in both keeps the union of configurations and the later
        // LastSeenSession.
        void Merge(const PipelineManifest& other);

        // Drop the least recently seen entries until at most `maxEntries` remain.
        void Trim(sizet maxEntries = kDefaultMaxEntries);

        [[nodiscard]] sizet GetEntryCount() const;
        [[nodiscard]] bool IsDirty() const
        {
            return m_Dirty;
        }
        void ClearDirty()
        {
            m_Dirty = false;
        }

        // Where the machine-wide manifest lives: beside the Vulkan pipeline
        // cache blob (ShaderCachePaths::Root()/vulkan/), so deleting that
        // directory resets both together and OLO_SHADER_CACHE_DIR relocates both.
        [[nodiscard]] static std::filesystem::path MachineFilePath();
        // Where a packaged game ships the editor's manifest as a seed (staged
        // by GameBuildPipeline next to ShaderPack.osp), relative to the
        // runtime's working directory.
        static constexpr std::string_view kPackagedSeedPath = "assets/PipelineManifest.yaml";

        [[nodiscard]] std::string Serialize() const;

        // Parse a serialized manifest. A malformed document is refused whole
        // (nullopt plus a reason); a well-formed document with individually
        // invalid entries keeps the valid ones and reports how many it dropped.
        [[nodiscard]] static std::optional<PipelineManifest> Parse(std::string_view text, std::string* error = nullptr,
                                                                   sizet* rejectedEntries = nullptr);

      private:
        [[nodiscard]] static std::string IdentityKey(std::string_view shader, u32 stageMask);

        // Identity key -> that shader's entries.
        std::unordered_map<std::string, TArray<PipelineManifestEntry>> m_ByShader;
        u64 m_Session = 0;
        bool m_Dirty = false;
    };

    // The configuration tag the renderer selects pipelines under: the settings
    // that change the attachment formats or sample counts a pass draws into.
    // One function, so the tag the renderer publishes and the one a test
    // expects cannot be spelled two ways.
    [[nodiscard]] std::string MakePipelineConfigurationTag(std::string_view renderingPath, u32 gbufferSamples,
                                                           std::string_view upscaleMode, bool temporalUpscale,
                                                           bool oit);
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <bit>
#include <format>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool IsValidSampleCount(const u32 samples)
        {
            return samples >= 1u && samples <= 64u && std::has_single_bit(samples);
        }

        [[nodiscard]] bool IsValidSignature(const PipelineSignature& signature)
        {
            return signature.ColorCount <= PipelineSignature::kMaxColorAttachments &&
                   IsValidSampleCount(signature.Samples) && signature.PatchControlPoints <= 32u;
        }
    } // namespace

    bool PipelineManifestEntry::SeenUnder(const std::string_view configuration) const
    {
        return Configurations.ContainsByPredicate([configuration](const FString& tag)
                                                  { return tag.ToView() == configuration; });
    }

    void PipelineManifest::BeginSession()
    {
        ++m_Session;
        m_Dirty = true;
    }

    std::string PipelineManifest::IdentityKey(const std::string_view shader, const u32 stageMask)
    {
        return std::format("{}#{:x}", shader, stageMask);
    }

    bool PipelineManifest::Record(const std::string_view shader, const u32 stageMask, const PipelineSignature& signature,
                                  const std::string_view configuration)
    {
        if (shader.empty() || !IsValidSignature(signature))
        {
            return false;
        }
        auto& entries = m_ByShader[IdentityKey(shader, stageMask)];
        PipelineManifestEntry* entry =
            entries.FindByPredicate([&signature](const PipelineManifestEntry& e) { return e.Signature == signature; });
        bool changed = false;
        if (entry == nullptr)
        {
            entry = &entries.Emplace_GetRef();
            entry->Shader = FString(shader);
            entry->StageMask = stageMask;
            entry->Signature = signature;
            changed = true;
        }
        if (!configuration.empty() && !entry->SeenUnder(configuration))
        {
            if (entry->Configurations.Num() >= static_cast<i32>(kMaxConfigurationsPerEntry))
            {
                entry->Configurations.RemoveAt(0); // oldest configuration goes first
            }
            entry->Configurations.Add(FString(configuration));
            changed = true;
        }
        if (entry->LastSeenSession != m_Session)
        {
            entry->LastSeenSession = m_Session;
            changed = true;
        }
        m_Dirty = m_Dirty || changed;
        return changed;
    }

    TArray<const PipelineManifestEntry*> PipelineManifest::EntriesFor(const std::string_view shader, const u32 stageMask,
                                                                      const std::string_view configuration) const
    {
        TArray<const PipelineManifestEntry*> result;
        const auto it = m_ByShader.find(IdentityKey(shader, stageMask));
        if (it == m_ByShader.end())
        {
            return result;
        }
        for (const PipelineManifestEntry& entry : it->second)
        {
            result.Add(&entry);
        }
        // Selected configuration first; stable so the record order (the order
        // the draws needed them) survives within each group.
        std::ranges::stable_sort(result, [configuration](const PipelineManifestEntry* a, const PipelineManifestEntry* b)
                                 { return a->SeenUnder(configuration) && !b->SeenUnder(configuration); });
        return result;
    }

    void PipelineManifest::Merge(const PipelineManifest& other)
    {
        m_Session = std::max(m_Session, other.m_Session);
        for (const auto& [identity, otherEntries] : other.m_ByShader)
        {
            auto& entries = m_ByShader[identity];
            for (const PipelineManifestEntry& incoming : otherEntries)
            {
                PipelineManifestEntry* entry = entries.FindByPredicate([&incoming](const PipelineManifestEntry& e)
                                                                       { return e.Signature == incoming.Signature; });
                if (entry == nullptr)
                {
                    entries.Add(incoming);
                    m_Dirty = true;
                    continue;
                }
                for (const FString& tag : incoming.Configurations)
                {
                    if (!entry->SeenUnder(tag.ToView()))
                    {
                        entry->Configurations.Add(tag);
                        m_Dirty = true;
                    }
                }
                entry->LastSeenSession = std::max(entry->LastSeenSession, incoming.LastSeenSession);
            }
        }
    }

    void PipelineManifest::Trim(const sizet maxEntries)
    {
        sizet count = GetEntryCount();
        if (count <= maxEntries)
        {
            return;
        }
        // Find the session cut-off below which entries go, then drop the oldest
        // until the bound holds. Ties at the cut-off are broken by position.
        TArray<u64> sessions;
        for (const auto& [identity, entries] : m_ByShader)
        {
            for (const PipelineManifestEntry& entry : entries)
            {
                sessions.Add(entry.LastSeenSession);
            }
        }
        std::ranges::sort(sessions);
        const u64 cutoff = sessions[static_cast<i32>(count - maxEntries - 1)];
        sizet toDrop = count - maxEntries;
        for (auto& [identity, entries] : m_ByShader)
        {
            for (i32 i = entries.Num() - 1; i >= 0 && toDrop > 0; --i)
            {
                if (entries[i].LastSeenSession <= cutoff)
                {
                    entries.RemoveAt(i);
                    --toDrop;
                }
            }
        }
        std::erase_if(m_ByShader, [](const auto& item) { return item.second.IsEmpty(); });
        m_Dirty = true;
    }

    sizet PipelineManifest::GetEntryCount() const
    {
        sizet count = 0;
        for (const auto& [identity, entries] : m_ByShader)
        {
            count += static_cast<sizet>(entries.Num());
        }
        return count;
    }

    std::filesystem::path PipelineManifest::MachineFilePath()
    {
        return ShaderCachePaths::Root() / "vulkan" / "pso_manifest.yaml";
    }

    std::string PipelineManifest::Serialize() const
    {
        // Deterministic order (identity key, then record order) so two saves of
        // the same manifest are byte-identical.
        TArray<const std::string*> identities;
        for (const auto& [identity, entries] : m_ByShader)
        {
            identities.Add(&identity);
        }
        std::ranges::sort(identities, [](const std::string* a, const std::string* b) { return *a < *b; });

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "PipelineManifest" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Version" << YAML::Value << kFormatVersion;
        out << YAML::Key << "Session" << YAML::Value << m_Session;
        out << YAML::Key << "Entries" << YAML::Value << YAML::BeginSeq;
        for (const std::string* identity : identities)
        {
            for (const PipelineManifestEntry& entry : m_ByShader.at(*identity))
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Shader" << YAML::Value << entry.Shader.ToStdString();
                out << YAML::Key << "Stages" << YAML::Value << entry.StageMask;
                out << YAML::Key << "Color" << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (u32 i = 0; i < entry.Signature.ColorCount; ++i)
                {
                    out << entry.Signature.ColorFormats[i];
                }
                out << YAML::EndSeq;
                out << YAML::Key << "Depth" << YAML::Value << entry.Signature.DepthFormat;
                out << YAML::Key << "Samples" << YAML::Value << entry.Signature.Samples;
                out << YAML::Key << "Patch" << YAML::Value << entry.Signature.PatchControlPoints;
                out << YAML::Key << "Configs" << YAML::Value << YAML::BeginSeq;
                for (const FString& tag : entry.Configurations)
                {
                    out << tag.ToStdString();
                }
                out << YAML::EndSeq;
                out << YAML::Key << "LastSeen" << YAML::Value << entry.LastSeenSession;
                out << YAML::EndMap;
            }
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
        out << YAML::EndMap;
        return out.c_str();
    }

    std::optional<PipelineManifest> PipelineManifest::Parse(const std::string_view text, std::string* error,
                                                            sizet* rejectedEntries)
    {
        const auto fail = [error](std::string reason) -> std::optional<PipelineManifest>
        {
            if (error != nullptr)
            {
                *error = std::move(reason);
            }
            return std::nullopt;
        };
        if (rejectedEntries != nullptr)
        {
            *rejectedEntries = 0;
        }

        YAML::Node root;
        try
        {
            root = YAML::Load(std::string(text));
        }
        catch (const YAML::Exception& e)
        {
            return fail(std::format("not YAML: {}", e.what()));
        }
        const YAML::Node manifestNode = root["PipelineManifest"];
        if (!manifestNode || !manifestNode.IsMap())
        {
            return fail("no PipelineManifest map");
        }

        PipelineManifest manifest;
        try
        {
            if (!manifestNode["Version"] || manifestNode["Version"].as<u32>() != kFormatVersion)
            {
                return fail(std::format("unsupported version (expected {})", kFormatVersion));
            }
            if (manifestNode["Session"])
            {
                manifest.m_Session = manifestNode["Session"].as<u64>();
            }
            const YAML::Node entries = manifestNode["Entries"];
            if (entries && !entries.IsSequence())
            {
                return fail("Entries is not a sequence");
            }
            for (const YAML::Node& node : entries)
            {
                // One bad entry is dropped, not the file: a hand-edited or
                // partially written manifest still precaches what it can.
                try
                {
                    PipelineManifestEntry entry;
                    entry.Shader = FString(node["Shader"].as<std::string>());
                    entry.StageMask = node["Stages"].as<u32>();
                    const YAML::Node colors = node["Color"];
                    if (!colors.IsSequence() || colors.size() > PipelineSignature::kMaxColorAttachments)
                    {
                        throw YAML::Exception(YAML::Mark::null_mark(), "bad Color");
                    }
                    entry.Signature.ColorCount = static_cast<u32>(colors.size());
                    for (u32 i = 0; i < entry.Signature.ColorCount; ++i)
                    {
                        entry.Signature.ColorFormats[i] = colors[i].as<u32>();
                    }
                    entry.Signature.DepthFormat = node["Depth"].as<u32>();
                    entry.Signature.Samples = node["Samples"].as<u32>();
                    entry.Signature.PatchControlPoints = node["Patch"] ? node["Patch"].as<u32>() : 0u;
                    if (const YAML::Node configs = node["Configs"]; configs && configs.IsSequence())
                    {
                        for (const YAML::Node& tag : configs)
                        {
                            entry.Configurations.Add(FString(tag.as<std::string>()));
                        }
                    }
                    entry.LastSeenSession = node["LastSeen"] ? node["LastSeen"].as<u64>() : 0u;
                    if (entry.Shader.IsEmpty() || !IsValidSignature(entry.Signature))
                    {
                        throw YAML::Exception(YAML::Mark::null_mark(), "invalid signature");
                    }
                    auto& list = manifest.m_ByShader[IdentityKey(entry.Shader.ToView(), entry.StageMask)];
                    if (!list.ContainsByPredicate([&entry](const PipelineManifestEntry& e)
                                                  { return e.Signature == entry.Signature; }))
                    {
                        list.Add(std::move(entry));
                    }
                }
                catch (const YAML::Exception&)
                {
                    if (rejectedEntries != nullptr)
                    {
                        ++*rejectedEntries;
                    }
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            return fail(std::format("malformed: {}", e.what()));
        }
        return manifest;
    }

    std::string MakePipelineConfigurationTag(const std::string_view renderingPath, const u32 gbufferSamples,
                                             const std::string_view upscaleMode, const bool temporalUpscale,
                                             const bool oit)
    {
        return std::format("{}/msaa{}/{}{}/oit{}", renderingPath, gbufferSamples, upscaleMode,
                           temporalUpscale ? "+temporal" : "", oit ? 1 : 0);
    }
} // namespace OloEngine

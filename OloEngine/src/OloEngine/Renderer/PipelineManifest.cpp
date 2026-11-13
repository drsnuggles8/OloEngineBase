#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <tuple>
#include <unordered_map>
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

        // Drops the oldest tags until at most `room` remain. A list read from
        // a file a previous build wrote can already be over the bound.
        void KeepNewestConfigurations(PipelineManifestEntry& entry, const sizet room)
        {
            while (entry.Configurations.Num() > static_cast<i32>(room))
            {
                entry.Configurations.RemoveAt(0);
            }
        }

        // Record, Merge and Parse all add a tag this way, so the bound cannot
        // be kept by one and missed by another: the oldest tag goes first.
        void AddConfiguration(PipelineManifestEntry& entry, FString tag)
        {
            KeepNewestConfigurations(entry, PipelineManifest::kMaxConfigurationsPerEntry - 1);
            entry.Configurations.Add(std::move(tag));
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
            entries.FindByPredicate([&signature](const PipelineManifestEntry& e)
                                    { return e.Signature == signature; });
        bool changed = false;
        if (entry == nullptr)
        {
            entry = &entries.Emplace_GetRef();
            entry->Shader = FString(shader);
            entry->StageMask = stageMask;
            entry->Signature = signature;
            changed = true;
        }
        // The empty tag is recorded like any other: a session that never
        // publishes one (2D only) selects it, and its shaders must be able to
        // become ready under it.
        if (!entry->SeenUnder(configuration))
        {
            AddConfiguration(*entry, FString(configuration));
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
                    PipelineManifestEntry& added = entries.Add_GetRef(incoming);
                    KeepNewestConfigurations(added, kMaxConfigurationsPerEntry);
                    m_Dirty = true;
                    continue;
                }
                for (const FString& tag : incoming.Configurations)
                {
                    if (!entry->SeenUnder(tag.ToView()))
                    {
                        AddConfiguration(*entry, tag);
                        m_Dirty = true;
                    }
                }
                entry->LastSeenSession = std::max(entry->LastSeenSession, incoming.LastSeenSession);
            }
        }
    }

    void PipelineManifest::Trim(const sizet maxEntries)
    {
        const sizet count = GetEntryCount();
        if (count <= maxEntries)
        {
            return;
        }
        // Every entry, oldest session first; the first (count - maxEntries)
        // go. A global order, not a per-shader cut-off, so an entry tied with
        // the cut-off can never be dropped while a strictly older one stays.
        struct EntryRef
        {
            u64 Session = 0;
            const std::string* Identity = nullptr;
            i32 Index = 0;
        };
        TArray<EntryRef> all;
        all.Reserve(static_cast<i32>(count));
        for (const auto& [identity, entries] : m_ByShader)
        {
            for (i32 i = 0; i < entries.Num(); ++i)
            {
                all.Add({ entries[i].LastSeenSession, &identity, i });
            }
        }
        // Ties broken by identity and position, never by hash-map order, so
        // every standard library trims the same entries from the same file.
        std::ranges::sort(all, [](const EntryRef& a, const EntryRef& b)
                          { return std::tie(a.Session, *a.Identity, a.Index) < std::tie(b.Session, *b.Identity, b.Index); });

        std::unordered_map<const std::string*, TArray<i32>> doomed;
        for (sizet i = 0; i < count - maxEntries; ++i)
        {
            const EntryRef& entry = all[static_cast<i32>(i)];
            doomed[entry.Identity].Add(entry.Index);
        }
        for (auto& [identity, indices] : doomed)
        {
            // Highest index first, so the earlier indices stay valid.
            std::ranges::sort(indices, std::greater<>{});
            auto& entries = m_ByShader[*identity];
            for (const i32 index : indices)
            {
                entries.RemoveAt(index);
            }
        }
        std::erase_if(m_ByShader, [](const auto& item)
                      { return item.second.IsEmpty(); });
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

    TArray<const PipelineManifestEntry*> PipelineManifest::SortedEntries() const
    {
        TArray<const std::string*> identities;
        for (const auto& [identity, entries] : m_ByShader)
        {
            identities.Add(&identity);
        }
        std::ranges::sort(identities, [](const std::string* a, const std::string* b)
                          { return *a < *b; });
        const auto signatureOrder = [](const PipelineManifestEntry* a, const PipelineManifestEntry* b)
        {
            const PipelineSignature& x = a->Signature;
            const PipelineSignature& y = b->Signature;
            return std::tie(x.ColorCount, x.ColorFormats, x.DepthFormat, x.Samples, x.PatchControlPoints) <
                   std::tie(y.ColorCount, y.ColorFormats, y.DepthFormat, y.Samples, y.PatchControlPoints);
        };
        TArray<const PipelineManifestEntry*> sorted;
        for (const std::string* identity : identities)
        {
            TArray<const PipelineManifestEntry*> group;
            for (const PipelineManifestEntry& entry : m_ByShader.at(*identity))
            {
                group.Add(&entry);
            }
            std::ranges::sort(group, signatureOrder);
            sorted.Append(group);
        }
        return sorted;
    }

    PipelineManifest PipelineManifest::NormalizedForSeed() const
    {
        PipelineManifest seed;
        seed.m_ByShader = m_ByShader;
        for (auto& [identity, entries] : seed.m_ByShader)
        {
            for (PipelineManifestEntry& entry : entries)
            {
                entry.LastSeenSession = 0;
                std::ranges::sort(entry.Configurations, [](const FString& a, const FString& b)
                                  { return a.ToView() < b.ToView(); });
            }
        }
        seed.m_Session = 0;
        seed.m_Dirty = false;
        return seed;
    }

    PipelineManifest PipelineManifest::SeenInSession(const u64 session) const
    {
        PipelineManifest seen;
        seen.m_Session = session;
        for (const auto& [identity, entries] : m_ByShader)
        {
            for (const PipelineManifestEntry& entry : entries)
            {
                if (entry.LastSeenSession == session)
                {
                    seen.m_ByShader[identity].Add(entry);
                }
            }
        }
        return seen;
    }

    TArray<PipelineManifestEntry> PipelineManifest::EntriesMissingFrom(const PipelineManifest& seed) const
    {
        TArray<PipelineManifestEntry> missing;
        for (const PipelineManifestEntry* entry : SortedEntries())
        {
            const auto it = seed.m_ByShader.find(IdentityKey(entry->Shader.ToView(), entry->StageMask));
            const bool covered = it != seed.m_ByShader.end() &&
                                 it->second.ContainsByPredicate([entry](const PipelineManifestEntry& e)
                                                                { return e.Signature == entry->Signature; });
            if (!covered)
            {
                missing.Add(*entry);
            }
        }
        return missing;
    }

    bool PipelineManifest::Remove(const std::string_view shader, const u32 stageMask, const PipelineSignature& signature)
    {
        const auto it = m_ByShader.find(IdentityKey(shader, stageMask));
        if (it == m_ByShader.end())
        {
            return false;
        }
        const i32 removed = it->second.RemoveAll([&signature](const PipelineManifestEntry& e)
                                                 { return e.Signature == signature; });
        if (it->second.IsEmpty())
        {
            m_ByShader.erase(it);
        }
        m_Dirty = m_Dirty || removed > 0;
        return removed > 0;
    }

    std::string PipelineManifest::Describe(const PipelineManifestEntry& entry)
    {
        std::string colors;
        for (u32 i = 0; i < entry.Signature.ColorCount; ++i)
        {
            colors += std::format("{}{}", i == 0 ? "" : ",", entry.Signature.ColorFormats[i]);
        }
        std::string configs;
        for (const FString& tag : entry.Configurations)
        {
            configs += std::format("{}{}", configs.empty() ? "" : " ", tag.ToView());
        }
        return std::format("{} stages={:#x} color=[{}] depth={} samples={} patch={} configs=[{}]", entry.Shader.ToView(),
                           entry.StageMask, colors, entry.Signature.DepthFormat, entry.Signature.Samples,
                           entry.Signature.PatchControlPoints, configs);
    }

    std::string PipelineManifest::Serialize() const
    {
        // Deterministic order (identity key, then signature) so two saves of
        // the same keys are byte-identical whatever order they were recorded in.
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "PipelineManifest" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Version" << YAML::Value << kFormatVersion;
        out << YAML::Key << "Session" << YAML::Value << m_Session;
        out << YAML::Key << "Entries" << YAML::Value << YAML::BeginSeq;
        for (const PipelineManifestEntry* entryPtr : SortedEntries())
        {
            const PipelineManifestEntry& entry = *entryPtr;
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
                            AddConfiguration(entry, FString(tag.as<std::string>()));
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

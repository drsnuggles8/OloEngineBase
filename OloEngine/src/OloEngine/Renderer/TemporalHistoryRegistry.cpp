#include "OloEnginePCH.h"

#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/RenderGraphDeclarationKey.h"
#include "OloEngine/Core/DebugLevers.h"

namespace OloEngine
{
    std::size_t TemporalHistoryKeyHash::operator()(const TemporalHistoryKey& key) const noexcept
    {
        std::size_t seed = static_cast<std::size_t>(key.Effect);
        const auto combine = [&seed](std::size_t value)
        { seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u); };
        combine(std::hash<u64>{}(key.View));
        combine(static_cast<std::size_t>(key.Resolution));
        combine(static_cast<std::size_t>(key.Plane));
        return seed;
    }

    TemporalHistoryAcquireResult TemporalHistoryRegistry::Acquire(
        const TemporalHistoryKey& key,
        const TemporalHistoryDescriptor& descriptor,
        TemporalHistoryDependency dependencies,
        std::string debugName)
    {
        OLO_CORE_ASSERT(descriptor.IsUsable(), "Temporal history descriptors must have usable dimensions and format");

        if (!debugName.empty())
        {
            if (const auto owner = m_DebugNameOwners.find(debugName);
                owner != m_DebugNameOwners.end() && owner->second != key)
            {
                OLO_CORE_ERROR("Temporal history debug name '{}' is already owned by another typed key", debugName);
                return {};
            }
        }

        if (const auto it = m_Indices.find(key); it != m_Indices.end())
        {
            Entry& entry = m_Entries[it->second];
            entry.AcquiredThisPopulate = true;
            const bool descriptorChanged = entry.Descriptor != descriptor;
            entry.Dependencies = dependencies;
            if (!debugName.empty())
            {
                if (debugName != entry.DebugName.ToView())
                {
                    m_DebugNameOwners.erase(entry.DebugName.ToStdString());
                    m_DebugNameOwners.emplace(debugName, key);
                }
                entry.DebugName = FString(debugName);
            }

            if (descriptorChanged)
            {
                entry.Descriptor = descriptor;
                entry.Generation = NextTemporalHistoryGeneration(entry.Generation);
                entry.Valid = false;
                entry.Texture.Reset();
                entry.LastInvalidation = TemporalHistoryInvalidationCause::DescriptorChanged;
                entry.PendingLineageBreak = TemporalHistoryInvalidationCause::DescriptorChanged;
                entry.Age = 0;
            }
            return {
                .Token = { it->second, entry.Generation },
                .DescriptorChanged = descriptorChanged,
            };
        }

        const u32 index = static_cast<u32>(m_Entries.Num());
        m_Entries.Add(Entry{
            .Key = key,
            .Descriptor = descriptor,
            .Dependencies = dependencies,
            .AcquiredThisPopulate = true,
            .DebugName = FString(debugName),
        });
        m_Indices.emplace(key, index);
        if (!m_Entries.Last().DebugName.IsEmpty())
            m_DebugNameOwners.emplace(m_Entries.Last().DebugName.ToStdString(), key);
        return {
            .Token = { index, 1 },
            .Created = true,
        };
    }

    TemporalHistoryAcquireResult TemporalHistoryRegistry::AcquireExternal(
        const TemporalHistoryKey& key,
        const TemporalHistoryDescriptor& descriptor,
        TemporalHistoryDependency dependencies,
        std::string debugName)
    {
        const TemporalHistoryAcquireResult result = Acquire(key, descriptor, dependencies, std::move(debugName));
        if (Entry* entry = Resolve(result.Token))
            entry->External = true;
        return result;
    }

    TemporalHistoryRegistry::Entry* TemporalHistoryRegistry::Resolve(TemporalHistoryToken token)
    {
        if (!token.IsValid() || token.Index >= m_Entries.Num())
            return nullptr;
        Entry& entry = m_Entries[token.Index];
        return entry.Generation == token.Generation ? &entry : nullptr;
    }

    const TemporalHistoryRegistry::Entry* TemporalHistoryRegistry::Resolve(TemporalHistoryToken token) const
    {
        if (!token.IsValid() || token.Index >= m_Entries.Num())
            return nullptr;
        const Entry& entry = m_Entries[token.Index];
        return entry.Generation == token.Generation ? &entry : nullptr;
    }

    bool TemporalHistoryRegistry::IsCurrent(TemporalHistoryToken token) const
    {
        return Resolve(token) != nullptr;
    }

    bool TemporalHistoryRegistry::IsValid(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry && entry->Valid;
    }

    TemporalHistoryToken TemporalHistoryRegistry::Find(const TemporalHistoryKey& key) const
    {
        const auto it = m_Indices.find(key);
        if (it == m_Indices.end())
            return {};
        return { it->second, m_Entries[it->second].Generation };
    }

    TemporalHistoryToken TemporalHistoryRegistry::Current(TemporalHistoryToken token) const
    {
        if (!token.IsValid() || token.Index >= static_cast<u32>(m_Entries.Num()))
            return {};
        return { token.Index, m_Entries[token.Index].Generation };
    }

    const TemporalHistoryDescriptor* TemporalHistoryRegistry::GetDescriptor(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry ? &entry->Descriptor : nullptr;
    }

    std::string_view TemporalHistoryRegistry::GetDebugName(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry ? entry->DebugName.ToView() : std::string_view{};
    }

    Ref<Texture2D> TemporalHistoryRegistry::GetTexture(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry ? entry->Texture : Ref<Texture2D>{};
    }

    bool TemporalHistoryRegistry::SetTexture(TemporalHistoryToken token, Ref<Texture2D> texture)
    {
        Entry* entry = Resolve(token);
        if (!entry)
            return false;
        entry->Texture = std::move(texture);
        entry->Valid = false;
        return true;
    }

    bool TemporalHistoryRegistry::MarkProduced(TemporalHistoryToken token)
    {
        Entry* entry = Resolve(token);
        if (!entry || (!entry->Texture && !entry->External))
            return false;
        // An external lineage has no extraction to latch its read at: the
        // frame read it exactly when it was valid as it is marked produced.
        if (entry->External)
            entry->ValidAtExtraction = entry->Valid;
        // The lineage continues only when this frame read a valid history and
        // nothing broke it since. Otherwise the frame starts a new one, which
        // carries the cause that ended the old: the pending break, or a copy
        // that never landed (valid before, invalid when this frame began).
        const bool continues = entry->ValidAtExtraction &&
                               entry->PendingLineageBreak == TemporalHistoryInvalidationCause::None;
        if (continues)
        {
            entry->Age = entry->Age == ~0u ? entry->Age : entry->Age + 1u;
        }
        else
        {
            entry->Age = 1u;
            entry->LineageCause = entry->PendingLineageBreak != TemporalHistoryInvalidationCause::None
                                      ? entry->PendingLineageBreak
                                      : TemporalHistoryInvalidationCause::CopyFailed;
            entry->PendingLineageBreak = TemporalHistoryInvalidationCause::None;
        }
        entry->Valid = true;
        entry->ValidAtExtraction = true;
        entry->LastInvalidation = TemporalHistoryInvalidationCause::None;
        return true;
    }

    bool TemporalHistoryRegistry::MarkCopyFailed(TemporalHistoryToken token)
    {
        Entry* entry = Resolve(token);
        if (!entry)
            return false;
        // RenderGraph calls this on every sink before the frame's copies, then
        // MarkProduced on each copy that lands, so latch what the frame read.
        entry->ValidAtExtraction = entry->Valid;
        entry->Valid = false;
        entry->LastInvalidation = TemporalHistoryInvalidationCause::CopyFailed;
        return true;
    }

    TemporalHistoryDependency TemporalHistoryRegistry::DependencyForCause(TemporalHistoryInvalidationCause cause)
    {
        switch (cause)
        {
            case TemporalHistoryInvalidationCause::CameraCut:
                return TemporalHistoryDependency::ViewTransform;
            case TemporalHistoryInvalidationCause::ProjectionChanged:
                return TemporalHistoryDependency::Projection;
            case TemporalHistoryInvalidationCause::ViewportResized:
                return TemporalHistoryDependency::Viewport;
            case TemporalHistoryInvalidationCause::DynamicResolutionChanged:
                return TemporalHistoryDependency::RenderScale;
            case TemporalHistoryInvalidationCause::SceneReset:
                return TemporalHistoryDependency::Scene;
            case TemporalHistoryInvalidationCause::SceneMutated:
                return TemporalHistoryDependency::SceneContent;
            case TemporalHistoryInvalidationCause::FeatureToggled:
                return TemporalHistoryDependency::FeatureState;
            case TemporalHistoryInvalidationCause::BackendChanged:
                return TemporalHistoryDependency::Backend;
            case TemporalHistoryInvalidationCause::JitterReset:
                return TemporalHistoryDependency::Jitter;
            default:
                return TemporalHistoryDependency::None;
        }
    }

    u32 TemporalHistoryRegistry::Invalidate(
        TemporalHistoryInvalidationCause cause,
        std::optional<TemporalHistoryEffect> effect)
    {
        // The #1348 negative control: every reset policy keeps the old lineage.
        if (Levers::FaultKeepStaleTemporalHistory())
            return 0;

        const TemporalHistoryDependency dependency = DependencyForCause(cause);
        u32 invalidated = 0;
        for (Entry& entry : m_Entries)
        {
            if (effect && entry.Key.Effect != *effect)
                continue;
            if (dependency != TemporalHistoryDependency::None &&
                (entry.Dependencies & dependency) == TemporalHistoryDependency::None)
            {
                continue;
            }

            entry.Generation = NextTemporalHistoryGeneration(entry.Generation);
            entry.Valid = false;
            entry.LastInvalidation = cause;
            entry.PendingLineageBreak = cause;
            entry.Age = 0;
            ++invalidated;
        }
        return invalidated;
    }

    bool TemporalHistoryRegistry::Release(const TemporalHistoryKey& key, TemporalHistoryInvalidationCause cause)
    {
        const auto it = m_Indices.find(key);
        if (it == m_Indices.end())
            return false;
        Entry& entry = m_Entries[it->second];
        if (!entry.Texture && !entry.Valid)
            return false;
        entry.Texture.Reset();
        entry.Generation = NextTemporalHistoryGeneration(entry.Generation);
        entry.Valid = false;
        entry.LastInvalidation = cause;
        entry.PendingLineageBreak = cause;
        entry.Age = 0;
        return true;
    }

    void TemporalHistoryRegistry::BeginPopulate()
    {
        for (Entry& entry : m_Entries)
            entry.AcquiredThisPopulate = false;
    }

    u32 TemporalHistoryRegistry::ReleaseUnacquired(TemporalHistoryInvalidationCause cause, TArray<FString>* releasedNames)
    {
        u32 released = 0;
        for (const Entry& entry : m_Entries)
        {
            if (entry.External || entry.AcquiredThisPopulate || (!entry.Texture && !entry.Valid))
                continue;
            const FString name = entry.DebugName;
            if (Release(entry.Key, cause))
            {
                ++released;
                if (releasedNames)
                    releasedNames->Add(name);
            }
        }
        return released;
    }

    bool TemporalHistoryRegistry::HoldsAny(TemporalHistoryEffect effect) const
    {
        for (const Entry& entry : m_Entries)
        {
            if (entry.Key.Effect == effect && (entry.Texture || entry.Valid))
                return true;
        }
        return false;
    }

    void TemporalHistoryRegistry::Clear()
    {
        m_Indices.clear();
        m_DebugNameOwners.clear();
        m_Entries.Reset();
    }

    u64 TemporalHistoryRegistry::ComputeValidityKey() const
    {
        RGDeclarationKey key;
        key.Add(static_cast<u64>(m_Entries.Num()));
        for (const Entry& entry : m_Entries)
        {
            if (entry.External)
                continue;
            key.Add(entry.Key.Effect);
            key.Add(entry.Key.View);
            key.Add(entry.Key.Resolution);
            key.Add(entry.Key.Plane);
            // Valid implies a texture (MarkProduced refuses without one), so the
            // texture's creation inside a populate adds nothing but a rebuild.
            key.Add(entry.Valid);
            // The descriptor too (#1348). A resize changes it INSIDE the
            // populate that acquires the history, after this frame's key was
            // captured with the history valid: the acquire drops it, the frame
            // extracts a new one and leaves it valid again, and without the
            // descriptor the next frame's key equals this one, so the cached
            // graph that skipped the import is served until something else
            // moves the key. SSR's history ran without history after every
            // upscale toggle that way (RendererStateMachineEvidence's
            // cached-vs-rebuild pair caught it).
            key.Add(entry.Descriptor.Width);
            key.Add(entry.Descriptor.Height);
            key.Add(entry.Descriptor.Format);
            key.Add(entry.Descriptor.MipLevels);
            key.Add(entry.Descriptor.Samples);
            key.Add(entry.Descriptor.LayoutVersion);
        }
        return key.Get();
    }

    TArray<TemporalHistorySnapshot> TemporalHistoryRegistry::Snapshot() const
    {
        TArray<TemporalHistorySnapshot> result;
        result.Reserve(m_Entries.Num());
        for (u32 index = 0; index < static_cast<u32>(m_Entries.Num()); ++index)
        {
            const Entry& entry = m_Entries[index];
            result.Add(TemporalHistorySnapshot{
                .Key = entry.Key,
                .Descriptor = entry.Descriptor,
                .Token = { index, entry.Generation },
                .Dependencies = entry.Dependencies,
                .LastInvalidation = entry.LastInvalidation,
                .Age = entry.Age,
                .LineageCause = entry.LineageCause,
                .Valid = entry.Valid,
                .HasTexture = static_cast<bool>(entry.Texture),
                .DebugName = entry.DebugName,
            });
        }
        return result;
    }
} // namespace OloEngine

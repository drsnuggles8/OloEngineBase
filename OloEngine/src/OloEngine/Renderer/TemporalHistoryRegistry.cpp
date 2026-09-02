#include "OloEnginePCH.h"

#include "OloEngine/Renderer/TemporalHistoryRegistry.h"

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

        if (const auto it = m_Indices.find(key); it != m_Indices.end())
        {
            Entry& entry = m_Entries[it->second];
            const bool descriptorChanged = entry.Descriptor != descriptor;
            entry.Dependencies = dependencies;
            if (!debugName.empty())
                entry.DebugName = std::move(debugName);

            if (descriptorChanged)
            {
                entry.Descriptor = descriptor;
                entry.Generation = NextTemporalHistoryGeneration(entry.Generation);
                entry.Valid = false;
                entry.Texture.Reset();
                entry.LastInvalidation = TemporalHistoryInvalidationCause::DescriptorChanged;
            }
            return {
                .Token = { it->second, entry.Generation },
                .DescriptorChanged = descriptorChanged,
            };
        }

        const u32 index = static_cast<u32>(m_Entries.size());
        m_Entries.push_back(Entry{
            .Key = key,
            .Descriptor = descriptor,
            .Dependencies = dependencies,
            .DebugName = std::move(debugName),
        });
        m_Indices.emplace(key, index);
        return {
            .Token = { index, 1 },
            .Created = true,
        };
    }

    TemporalHistoryRegistry::Entry* TemporalHistoryRegistry::Resolve(TemporalHistoryToken token)
    {
        if (!token.IsValid() || token.Index >= m_Entries.size())
            return nullptr;
        Entry& entry = m_Entries[token.Index];
        return entry.Generation == token.Generation ? &entry : nullptr;
    }

    const TemporalHistoryRegistry::Entry* TemporalHistoryRegistry::Resolve(TemporalHistoryToken token) const
    {
        if (!token.IsValid() || token.Index >= m_Entries.size())
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

    const TemporalHistoryDescriptor* TemporalHistoryRegistry::GetDescriptor(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry ? &entry->Descriptor : nullptr;
    }

    std::string_view TemporalHistoryRegistry::GetDebugName(TemporalHistoryToken token) const
    {
        const Entry* entry = Resolve(token);
        return entry ? std::string_view(entry->DebugName) : std::string_view{};
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
        if (!entry || !entry->Texture)
            return false;
        entry->Valid = true;
        entry->LastInvalidation = TemporalHistoryInvalidationCause::None;
        return true;
    }

    bool TemporalHistoryRegistry::MarkCopyFailed(TemporalHistoryToken token)
    {
        Entry* entry = Resolve(token);
        if (!entry)
            return false;
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
            ++invalidated;
        }
        return invalidated;
    }

    void TemporalHistoryRegistry::Clear()
    {
        m_Indices.clear();
        m_Entries.clear();
    }

    std::vector<TemporalHistorySnapshot> TemporalHistoryRegistry::Snapshot() const
    {
        std::vector<TemporalHistorySnapshot> result;
        result.reserve(m_Entries.size());
        for (u32 index = 0; index < static_cast<u32>(m_Entries.size()); ++index)
        {
            const Entry& entry = m_Entries[index];
            result.push_back(TemporalHistorySnapshot{
                .Key = entry.Key,
                .Descriptor = entry.Descriptor,
                .Token = { index, entry.Generation },
                .Dependencies = entry.Dependencies,
                .LastInvalidation = entry.LastInvalidation,
                .Valid = entry.Valid,
                .HasTexture = static_cast<bool>(entry.Texture),
                .DebugName = entry.DebugName,
            });
        }
        return result;
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <algorithm>
#include <cstdlib>

namespace OloEngine::RHI
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }
    } // namespace

    auto DescriptorHeap::Get() -> DescriptorHeap&
    {
        // Leaked for the same reason ResourceRegistry::Get() is: a view can be
        // destroyed during static destruction, and unregistering into a
        // destroyed heap would be a use-after-free of the very thing meant to
        // make lifetimes checkable.
        static DescriptorHeap* s_Instance = new DescriptorHeap();
        return *s_Instance;
    }

    void DescriptorHeap::Initialize(const HeapDesc& desc, IDescriptorHeapBackend* backend)
    {
        const std::lock_guard lock(m_Mutex);

        // Re-initialising must retire everything first, or a view minted against
        // the previous device stays structurally valid and resolves into the new
        // one's slots — the recycled-name hazard, one level up.
        for (u32 index = 0u; index < static_cast<u32>(m_Slots.size()); ++index)
        {
            if (m_Slots[index].Live)
            {
                ReleaseSlotLocked(index);
            }
        }

        m_Desc = desc;
        m_Backend = backend;
        m_PersistentCapacity = desc.ResourceSlotCapacity;
        m_TransientCapacity = desc.FrameTransientRingSlots;

        const u32 total = m_PersistentCapacity + m_TransientCapacity;
        m_Slots.assign(total, ViewSlot{});
        m_Mirror.assign(total, kPoisonDescriptor);

        m_PersistentFreeList.clear();
        m_PersistentFreeList.reserve(m_PersistentCapacity);
        // Descending, so the first allocations come off the front of the heap in
        // ascending order. Purely a debuggability choice: a capture that shows
        // "the shadow atlas is offset 3" every run is worth more than one where
        // the offsets shuffle between runs.
        //
        // SLOT 0 IS NEVER ALLOCATED. It is the null descriptor, permanently
        // poisoned, and it exists because "unbind" does not survive the
        // translation to a heap. A slot-based pass clears an input by binding a
        // null texture; under the heap there is nothing to bind — the OFFSET is
        // what the shader reads, and leaving it alone would keep sampling the
        // previous frame's texture through a perfectly valid offset. Writing
        // kNullHeapOffset instead gives that call site somewhere honest to point.
        // (ToneMapRenderPass does exactly this with RHI::NullResource, which is
        // how the hazard was found.)
        for (u32 index = m_PersistentCapacity; index > kNullHeapOffset + 1u; --index)
        {
            m_PersistentFreeList.push_back(index - 1u);
        }

        m_TransientCursor = 0u;
        m_SamplerSlots.clear();
        m_ViewsByResource.clear();
        m_PersistentViewCache.clear();
        m_DirtyFirst = 0u;
        m_DirtyLast = 0u;

        ++m_InitEpoch;

        m_Stats = Stats{};
        m_Stats.PersistentCapacity = m_PersistentCapacity;
        m_Stats.TransientCapacity = m_TransientCapacity;

        m_Initialized = (backend != nullptr) && total > 0u;

        // The toggle defaults from the environment so an A/B capture needs no
        // rebuild, and defaults OFF so that a machine without the extension —
        // and every headless test — takes the slot-based path it always took.
        m_Enabled = m_Initialized && backend->IsBindlessSupported() &&
                    IsTruthyEnvironmentVariable("OLO_RHI_BINDLESS");

        if (m_Initialized)
        {
            OLO_CORE_INFO("[RHI] Descriptor heap initialised: {} persistent + {} transient slots, "
                          "bindless {}, poison {}",
                          m_PersistentCapacity, m_TransientCapacity,
                          m_Enabled ? "ENABLED" : (backend->IsBindlessSupported() ? "available (off)" : "UNSUPPORTED"),
                          m_Desc.PoisonOnFree ? "on" : "off");
        }
    }

    void DescriptorHeap::Shutdown()
    {
        const std::lock_guard lock(m_Mutex);

        for (u32 index = 0u; index < static_cast<u32>(m_Slots.size()); ++index)
        {
            if (m_Slots[index].Live)
            {
                ReleaseSlotLocked(index);
            }
        }

        ++m_InitEpoch;
        m_Initialized = false;
        m_Enabled = false;
        m_Backend = nullptr;
        m_Slots.clear();
        m_Mirror.clear();
        m_PersistentFreeList.clear();
        m_SamplerSlots.clear();
        m_ViewsByResource.clear();
        m_PersistentViewCache.clear();
        m_TransientCursor = 0u;
        m_DirtyFirst = 0u;
        m_DirtyLast = 0u;
        m_Stats.PersistentLive = 0u;
        m_Stats.TransientThisFrame = 0u;
        m_Stats.SamplerSlotsLive = 0u;
    }

    auto DescriptorHeap::IsEnabled() const -> bool
    {
        const std::lock_guard lock(m_Mutex);
        return m_Enabled;
    }

    void DescriptorHeap::SetEnabled(bool enabled)
    {
        const std::lock_guard lock(m_Mutex);
        if (!m_Initialized || m_Backend == nullptr || !m_Backend->IsBindlessSupported())
        {
            // Silently staying disabled would make "I set it and nothing
            // happened" a mystery on exactly the machines where it matters.
            if (enabled)
            {
                OLO_CORE_WARN("[RHI] Bindless requested but unavailable (initialised={}, supported={}). "
                              "Staying on the slot-based path.",
                              m_Initialized, m_Backend != nullptr && m_Backend->IsBindlessSupported());
            }
            m_Enabled = false;
            return;
        }

        m_Enabled = enabled;
    }

    auto DescriptorHeap::IsPoisonOnFree() const -> bool
    {
        const std::lock_guard lock(m_Mutex);
        return m_Desc.PoisonOnFree;
    }

    auto DescriptorHeap::GetInitEpoch() const -> u64
    {
        const std::lock_guard lock(m_Mutex);
        return m_InitEpoch;
    }

    // -------------------------------------------------------------------------
    // Views
    // -------------------------------------------------------------------------

    auto DescriptorHeap::CreateView(ResourceHandle resource, const ViewDesc& view, const SamplerDesc& sampler,
                                    HeapSlotLifetime lifetime) -> ViewHandle
    {
        const std::lock_guard lock(m_Mutex);

        if (!m_Enabled)
        {
            return {};
        }

        // A view of a dead resource is not an error worth asserting on — a pass
        // can legitimately ask for one during teardown — but it must not produce
        // a live offset, because the descriptor behind it would name nothing.
        if (!ResourceRegistry::Get().IsLive(resource))
        {
            ++m_Stats.DescriptorFailures;
            return {};
        }

        u32 index = 0u;
        if (lifetime == HeapSlotLifetime::Persistent)
        {
            if (m_PersistentFreeList.empty())
            {
                ++m_Stats.PersistentOverflows;
                OLO_CORE_WARN("[RHI] Descriptor heap persistent region exhausted ({} slots). "
                              "Caller falls back to the slot-based path.",
                              m_PersistentCapacity);
                return {};
            }
            index = m_PersistentFreeList.back();
            m_PersistentFreeList.pop_back();
        }
        else
        {
            if (m_TransientCursor >= m_TransientCapacity)
            {
                // The ring is a per-FRAME budget, so an overflow means this
                // frame asked for more transient views than the heap was sized
                // for — recoverable (the caller binds the old way) but worth
                // seeing, because the alternative design (grow the ring) would
                // silently reintroduce mid-frame offset rewriting.
                ++m_Stats.TransientOverflows;
                return {};
            }
            index = m_PersistentCapacity + m_TransientCursor;
            ++m_TransientCursor;
            m_Stats.TransientThisFrame = m_TransientCursor;
            m_Stats.TransientHighWater = std::max(m_Stats.TransientHighWater, m_TransientCursor);
        }

        const u64 descriptor = m_Backend->AcquireDescriptor(resource, view, sampler);
        if (descriptor == kPoisonDescriptor)
        {
            // The backend could not realise this view. Hand the slot back rather
            // than publishing a poisoned descriptor under a live view handle —
            // a live handle whose offset reads as black is much harder to
            // diagnose than no handle at all.
            ++m_Stats.DescriptorFailures;
            if (lifetime == HeapSlotLifetime::Persistent)
            {
                m_PersistentFreeList.push_back(index);
            }
            else
            {
                // Rewind the ring cursor. Safe because the lock is still held, so
                // nothing else can have taken the slot — and necessary because a
                // run of failures would otherwise exhaust the frame's transient
                // budget and start reporting overflows for a reason unrelated to
                // the frame's actual resource count.
                --m_TransientCursor;
                m_Stats.TransientThisFrame = m_TransientCursor;
            }
            return {};
        }

        ViewSlot& slot = m_Slots[index];

        // Any change invalidates every outstanding handle for this slot; +1 is
        // the cheapest change. Generation 0 is reserved for "never handed out"
        // (`Handle::IsValid` rejects it), so a wrap skips it.
        u32 generation = slot.Generation + 1u;
        if (generation == 0u)
        {
            generation = 1u;
        }

        slot.Generation = generation;
        slot.Live = true;
        slot.Lifetime = lifetime;
        slot.Resource = resource;
        slot.View = view;
        slot.SamplerSlot = AcquireSamplerSlotLocked(sampler);
        slot.Descriptor = descriptor;

        m_Mirror[index] = descriptor;
        MarkDirtyLocked(index);

        m_ViewsByResource[resource.Index].push_back(index);

        ++m_Stats.ViewsCreated;
        if (lifetime == HeapSlotLifetime::Persistent)
        {
            ++m_Stats.PersistentLive;
        }

        return ViewHandle{ index, generation };
    }

    auto DescriptorHeap::GetOrCreateView(ResourceHandle resource, const ViewDesc& view, const SamplerDesc& sampler,
                                         HeapSlotLifetime lifetime) -> ViewHandle
    {
        if (lifetime == HeapSlotLifetime::FrameTransient)
        {
            // Never memoised — see the header. Two acquisitions of one physical
            // object within a frame MUST get two offsets, because that is how an
            // alias becomes visible in the heap instead of needing a mid-frame
            // rewrite.
            return CreateView(resource, view, sampler, lifetime);
        }

        // The sampler slot is resolved first so the cache key can carry it: two
        // views that differ only in sampler state are different views, and the
        // dedup makes that comparison an integer rather than a struct compare.
        u32 samplerSlot = 0u;
        {
            const std::lock_guard lock(m_Mutex);
            if (!m_Enabled)
            {
                return {};
            }
            samplerSlot = AcquireSamplerSlotLocked(sampler);
            // Acquired only to read its index; the view below takes its own
            // reference, so release this one or the refcount leaks by one per
            // lookup and the slot can never be reused.
            ReleaseSamplerSlotLocked(samplerSlot);

            const PersistentViewKey key{ .Resource = HashKey(resource),
                                         .SamplerSlot = samplerSlot,
                                         .DepthCompare = view.DepthCompare };
            if (const auto it = m_PersistentViewCache.find(key); it != m_PersistentViewCache.end())
            {
                // Revalidate rather than trust. The entry survives the resource
                // being destroyed (nothing tells the cache), so a stale hit must
                // fall through to a fresh mint instead of handing back a view
                // whose slot now belongs to someone else.
                if (ValidateLocked(it->second) != nullptr)
                {
                    return it->second;
                }
                m_PersistentViewCache.erase(it);
            }
        }

        const ViewHandle created = CreateView(resource, view, sampler, lifetime);
        if (created.IsValid())
        {
            const std::lock_guard lock(m_Mutex);
            m_PersistentViewCache[PersistentViewKey{ .Resource = HashKey(resource),
                                                     .SamplerSlot = samplerSlot,
                                                     .DepthCompare = view.DepthCompare }] = created;
        }
        return created;
    }

    void DescriptorHeap::DestroyView(ViewHandle view)
    {
        const std::lock_guard lock(m_Mutex);

        const ViewSlot* slot = ValidateLocked(view);
        if (slot == nullptr)
        {
            return;
        }

        if (slot->Lifetime == HeapSlotLifetime::FrameTransient)
        {
            // Destroying a transient by hand would return its ring index to
            // nobody (the ring is a cursor, not a freelist) and would retire a
            // generation the frame boundary is about to retire anyway. Counted
            // as a stale rejection so the mistake is visible in the stats rather
            // than being a silent no-op.
            ++m_Stats.StaleOffsetRejections;
            return;
        }

        ReleaseSlotLocked(view.Index);
        m_PersistentFreeList.push_back(view.Index);
        --m_Stats.PersistentLive;
    }

    auto DescriptorHeap::OffsetOf(ViewHandle view) const -> HeapOffset
    {
        const std::lock_guard lock(m_Mutex);

        const ViewSlot* slot = ValidateLocked(view);
        if (slot == nullptr)
        {
            return {};
        }

        // View index IS the resource-heap slot index — see the header. The
        // lookup exists for the generation compare, not for an indirection.
        return HeapOffset{ view.Index };
    }

    auto DescriptorHeap::SamplerOffsetOf(ViewHandle view) const -> HeapOffset
    {
        const std::lock_guard lock(m_Mutex);

        const ViewSlot* slot = ValidateLocked(view);
        if (slot == nullptr)
        {
            return {};
        }

        return HeapOffset{ slot->SamplerSlot };
    }

    auto DescriptorHeap::IsLive(ViewHandle view) const -> bool
    {
        const std::lock_guard lock(m_Mutex);
        return ValidateLocked(view) != nullptr;
    }

    auto DescriptorHeap::LifetimeOf(ViewHandle view) const -> HeapSlotLifetime
    {
        const std::lock_guard lock(m_Mutex);

        const ViewSlot* slot = ValidateLocked(view);
        // Persistent is the safe answer for an unknown view: it is the class
        // whose offsets a caller is allowed to cache, so reporting it for a dead
        // handle would be the dangerous direction — but a dead handle's offset
        // is already invalid, so nothing can act on this without first failing
        // OffsetOf.
        return slot != nullptr ? slot->Lifetime : HeapSlotLifetime::Persistent;
    }

    // -------------------------------------------------------------------------
    // Frame boundary and publication
    // -------------------------------------------------------------------------

    void DescriptorHeap::ResetFrameTransients()
    {
        const std::lock_guard lock(m_Mutex);

        for (u32 offset = 0u; offset < m_TransientCursor; ++offset)
        {
            const u32 index = m_PersistentCapacity + offset;
            if (m_Slots[index].Live)
            {
                ReleaseSlotLocked(index);
            }
        }

        m_TransientCursor = 0u;
        m_Stats.TransientThisFrame = 0u;
    }

    void DescriptorHeap::Flush()
    {
        const std::lock_guard lock(m_Mutex);

        if (!m_Enabled || m_Backend == nullptr)
        {
            return;
        }

        if (m_DirtyLast > m_DirtyFirst)
        {
            m_Backend->UploadSlots(m_DirtyFirst, m_Mirror.data() + m_DirtyFirst, m_DirtyLast - m_DirtyFirst);
            m_DirtyFirst = 0u;
            m_DirtyLast = 0u;
        }

        // Unconditional, even on a clean frame. A pass that unbinds the heap's
        // binding point for its own buffer would otherwise leave the next
        // frame's shaders indexing whatever is there — and unlike a missing
        // texture bind, that reads as a plausible frame rather than an obvious
        // one, because the offsets are still perfectly valid indices.
        m_Backend->BindHeap();
    }

    void DescriptorHeap::InvalidateResource(ResourceHandle resource)
    {
        const std::lock_guard lock(m_Mutex);

        if (!m_Enabled || m_Backend == nullptr)
        {
            return;
        }

        const auto it = m_ViewsByResource.find(resource.Index);
        if (it == m_ViewsByResource.end())
        {
            return;
        }

        for (const u32 index : it->second)
        {
            ViewSlot& slot = m_Slots[index];
            if (!slot.Live || slot.Resource != resource)
            {
                continue;
            }

            // Drop the stale descriptor BEFORE acquiring the replacement. The
            // opposite order looks safer and is not: `ARB_bindless_texture`
            // refuses to make an already-resident handle resident again, and the
            // recreated storage can be handed the same driver name — so
            // acquire-then-release can hand back a descriptor for the object it
            // is about to drop residency for.
            m_Backend->ReleaseDescriptor(slot.Descriptor);

            const SamplerDesc sampler = slot.SamplerSlot < static_cast<u32>(m_SamplerSlots.size())
                                            ? m_SamplerSlots[slot.SamplerSlot].Desc
                                            : SamplerDesc{};
            const u64 descriptor = m_Backend->AcquireDescriptor(slot.Resource, slot.View, sampler);
            if (descriptor == kPoisonDescriptor)
            {
                // Deliberately NOT the same policy as CreateView, which hands
                // the slot back rather than publishing poison under a live view.
                // Here the view already exists and callers may legitimately hold
                // its offset (persistent offsets are cacheable — ADR 0011 §1.2),
                // so retiring it would invalidate a handle that did nothing
                // wrong. Poisoning renders black, which is the honest answer to
                // "the resource behind this view could not be re-described".
                ++m_Stats.DescriptorFailures;
            }

            slot.Descriptor = descriptor;
            m_Mirror[index] = descriptor;
            MarkDirtyLocked(index);
        }
    }

    // -------------------------------------------------------------------------
    // Internals
    // -------------------------------------------------------------------------

    auto DescriptorHeap::ValidateLocked(ViewHandle view) const -> const ViewSlot*
    {
        if (!view.IsValid() || view.Index >= static_cast<u32>(m_Slots.size()))
        {
            ++m_Stats.StaleOffsetRejections;
            return nullptr;
        }

        const ViewSlot& slot = m_Slots[view.Index];
        if (!slot.Live || slot.Generation != view.Generation)
        {
            ++m_Stats.StaleOffsetRejections;
            return nullptr;
        }

        return &slot;
    }

    void DescriptorHeap::ReleaseSlotLocked(u32 index)
    {
        ViewSlot& slot = m_Slots[index];
        if (!slot.Live)
        {
            return;
        }

        if (m_Backend != nullptr)
        {
            m_Backend->ReleaseDescriptor(slot.Descriptor);
        }

        ReleaseSamplerSlotLocked(slot.SamplerSlot);

        // Advance the generation so every outstanding ViewHandle for this slot
        // becomes permanently stale. For a frame-transient view this is what
        // ENFORCES ADR 0011 §1.2's "never stored across a use": a transient
        // offset held into the next frame does not merely go stale, it is
        // DETECTABLY stale, and OffsetOf answers invalid rather than handing
        // back an index that now belongs to someone else.
        u32 generation = slot.Generation + 1u;
        if (generation == 0u)
        {
            generation = 1u;
        }

        if (const auto it = m_ViewsByResource.find(slot.Resource.Index); it != m_ViewsByResource.end())
        {
            std::erase(it->second, index);
            if (it->second.empty())
            {
                m_ViewsByResource.erase(it);
            }
        }

        slot.Generation = generation;
        slot.Live = false;
        slot.Resource = {};
        slot.View = {};
        slot.SamplerSlot = HeapOffset::Invalid;
        slot.Descriptor = kPoisonDescriptor;

        if (m_Desc.PoisonOnFree)
        {
            // Overwrite the published table too, not just the bookkeeping. The
            // whole value of poison is that a use-after-free renders
            // deterministically wrong instead of showing the previous tenant —
            // which is what LIFO slot reuse would otherwise hide in steady
            // state, exactly as the transient POOL hides it
            // (docs/agent-rules/render-graph-transient-aliasing.md).
            m_Mirror[index] = kPoisonDescriptor;
            MarkDirtyLocked(index);
            ++m_Stats.SlotsPoisoned;
        }
    }

    auto DescriptorHeap::AcquireSamplerSlotLocked(const SamplerDesc& sampler) -> u32
    {
        // Deduplicated by VALUE — ADR 0011 §1.2a. On OpenGL, filter and wrap
        // state lives on the texture object, so the engine has no shareable
        // sampler concept to convert; this is new machinery whose only consumer
        // today is the contract test, and it exists so the Vulkan sampler heap
        // arrives with a proven allocator rather than an invented one.
        //
        // The comparison is `SamplerDesc`'s defaulted `operator==`, which
        // includes the `f32 MaxAnisotropy` member. That is EXACT float equality,
        // and it is correct here rather than a violation of the float-comparison
        // rule: these are cache KEYS copied verbatim from a caller's literal, not
        // computed quantities, so two descs that should share a slot are
        // bit-identical and two that should not are visibly different. An epsilon
        // compare would make slot identity depend on iteration order.
        for (u32 index = 0u; index < static_cast<u32>(m_SamplerSlots.size()); ++index)
        {
            if (m_SamplerSlots[index].Desc == sampler)
            {
                ++m_SamplerSlots[index].RefCount;
                return index;
            }
        }

        if (static_cast<u32>(m_SamplerSlots.size()) >= m_Desc.SamplerSlotCapacity)
        {
            // Sharing slot 0 is wrong-but-bounded, and it is the only failure
            // here that does not cost correctness on THIS backend: under
            // ARB_bindless_texture the sampler state is baked into the texture
            // handle, so the sampler slot is bookkeeping. A Vulkan backend must
            // treat this as a hard error instead.
            OLO_CORE_WARN("[RHI] Sampler heap exhausted ({} slots); reusing slot 0. "
                          "Harmless on OpenGL (sampler state is baked into the texture handle), "
                          "NOT harmless on a split-heap backend.",
                          m_Desc.SamplerSlotCapacity);
            return 0u;
        }

        m_SamplerSlots.push_back(SamplerSlot{ .Desc = sampler, .RefCount = 1u });
        m_Stats.SamplerSlotsLive = static_cast<u32>(m_SamplerSlots.size());
        return static_cast<u32>(m_SamplerSlots.size()) - 1u;
    }

    void DescriptorHeap::ReleaseSamplerSlotLocked(u32 samplerSlot)
    {
        if (samplerSlot >= static_cast<u32>(m_SamplerSlots.size()))
        {
            return;
        }

        SamplerSlot& slot = m_SamplerSlots[samplerSlot];
        if (slot.RefCount > 0u)
        {
            --slot.RefCount;
        }

        // Slots are NOT compacted when the refcount hits zero. Compacting would
        // move every later sampler slot and therefore change offsets that live
        // views already published — the exact mid-frame offset rewrite the
        // transient ring exists to avoid, reintroduced in the sampler heap.
        // A zero-refcount slot is simply reusable by the next matching desc.
    }

    void DescriptorHeap::MarkDirtyLocked(u32 index)
    {
        if (m_DirtyLast <= m_DirtyFirst)
        {
            m_DirtyFirst = index;
            m_DirtyLast = index + 1u;
            return;
        }

        // One coalesced range rather than a set of indices. The heap is written
        // in bursts (a frame's transient views are contiguous by construction,
        // and persistent views are created at load time), so the span is tight
        // in practice and the upload is one call instead of N.
        m_DirtyFirst = std::min(m_DirtyFirst, index);
        m_DirtyLast = std::max(m_DirtyLast, index + 1u);
    }

    auto DescriptorHeap::GetStats() const -> Stats
    {
        const std::lock_guard lock(m_Mutex);
        Stats stats = m_Stats;
        stats.SamplerSlotsLive = static_cast<u32>(m_SamplerSlots.size());
        return stats;
    }

    void DescriptorHeap::ResetCounters()
    {
        const std::lock_guard lock(m_Mutex);
        m_Stats.StaleOffsetRejections = 0u;
        m_Stats.TransientOverflows = 0u;
        m_Stats.PersistentOverflows = 0u;
        m_Stats.DescriptorFailures = 0u;
        m_Stats.SlotsPoisoned = 0u;
        m_Stats.ViewsCreated = 0u;
        m_Stats.TransientHighWater = 0u;
    }

    // -------------------------------------------------------------------------
    // The free function Phase 1 declared in RHIResources.h.
    //
    // Declared there and left undefined — the same state `GetNativeHandleForDebug`
    // was in before Phase 2 step 3 defined it in the registry. Defined here
    // because the heap is the only thing that can answer it.
    //
    // The spelling is `RHI::OffsetOf`, not `Heap::OffsetOf`. ADR 0011 and the
    // Phase 3 handover both say `Heap::OffsetOf`, but the DECLARATION Phase 1
    // actually left behind is a namespace-scope free function, and implementing
    // the declared vocabulary rather than the described one is the lesson Phase 2
    // step 2 paid for (`RHI::MemoryResidency` was nearly reinvented as
    // `BufferUsage`). The member `DescriptorHeap::OffsetOf` exists for callers
    // that already hold a heap reference; this is the one the ADR's call sites
    // name.
    // -------------------------------------------------------------------------
    auto OffsetOf(ViewHandle view) -> HeapOffset
    {
        return DescriptorHeap::Get().OffsetOf(view);
    }
} // namespace OloEngine::RHI

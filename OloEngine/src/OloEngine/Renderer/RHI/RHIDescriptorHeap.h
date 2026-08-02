#pragma once

// =============================================================================
// RHIDescriptorHeap.h — the view registry and the shader-visible descriptor heap.
//
// Issue #691 Phase 3, ADR 0011 §1.1 / §1.2 / §1.2a.
//
// Phase 1 declared `RHI::ViewHandle` and `RHI::HeapOffset` and specified what
// they mean; Phase 2 amendment (11) deferred them to this phase "as a matched
// pair", on the grounds that a `ViewHandle` with no heap behind it detects
// nothing and a `HeapOffset` with no heap is a `u32` with a wrapper. This is
// the heap that makes both real.
//
// WHAT THIS IS FOR. It is the engine-side half of heap-bindless binding: a
// global table of GPU-resident descriptors, one slot per VIEW, indexed by a
// `HeapOffset` that travels to the shader as ordinary data in a UBO/SSBO
// instead of being re-established every frame by a bind call. The OpenGL
// backend realises it with `ARB_bindless_texture`; a Vulkan backend would
// realise it with `VK_EXT_descriptor_heap`. Those two mechanisms have almost
// nothing in common below this header — which is exactly why the *contract*
// above it is worth rehearsing on the backend we can still debug easily.
//
// THREE INVARIANTS, all of them load-bearing rather than decorative:
//
//   1. A SLOT IS OWNED BY A VIEW, NOT BY A RESOURCE. One resource maps to many
//      views and therefore many offsets. The engine already proves this:
//      `CreateDepthArrayCompareOffViewHandle` exists so one depth array can be
//      read both as a hardware-comparison shadow sampler and as a plain array
//      for the PCSS blocker search.
//
//   2. THERE ARE EXACTLY TWO LIFETIME CLASSES (ADR 0011 §1.2 is emphatic — do
//      not add a third). `Persistent` slots live from view creation to view
//      destruction. `FrameTransient` slots come from a per-frame ring that
//      resets at the frame boundary, because `TransientPool` hands the SAME
//      physical object to two logical resources with disjoint lifetimes and
//      `RenderGraph::WriteNewVersion` renames one physical resource. Under a
//      ring, an aliased pair gets two offsets onto one object — which makes the
//      alias VISIBLE in the heap rather than requiring an offset to be
//      rewritten mid-frame, the stale-read archetype
//      docs/agent-rules/render-graph-transient-aliasing.md warns about.
//
//   3. `OffsetOf` VALIDATES, AND THAT IS THE WHOLE POINT. GL recycles object
//      names and this heap recycles slot indices, so a bare `u32` cannot tell a
//      live offset from a dead one. Fetch the offset where you write it into a
//      buffer; the generation compare is cheap CPU-side insurance against a
//      failure mode that, under a real descriptor-heap backend, does not merely
//      sample the wrong resource but can hang the GPU.
//
// This header is API-neutral and stays that way (RHIBoundaryRatchetTest pins
// it): everything backend-specific reaches the heap through
// `IDescriptorHeapBackend` below, which is implemented in `Platform/<Backend>/`
// and installed at renderer init.
// =============================================================================

#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace OloEngine::RHI
{
    // -------------------------------------------------------------------------
    // The backend's half of the heap.
    //
    // Deliberately tiny, and deliberately expressed in engine types only. The
    // heap above owns slot ALLOCATION, lifetime, generation validation and
    // poisoning — all of which are backend-independent bookkeeping and all of
    // which are where the interesting bugs live. The backend owns only the
    // question "what descriptor does this view produce, and how do I publish
    // it", which is the part that genuinely differs between
    // `ARB_bindless_texture` and `VK_EXT_descriptor_heap`.
    // -------------------------------------------------------------------------
    class IDescriptorHeapBackend
    {
      public:
        virtual ~IDescriptorHeapBackend() = default;

        // False when the device cannot do heap-bindless at all. The heap then
        // stays disabled and every caller falls back to the slot-based binding
        // path — this is a REHEARSAL on the primary backend, so it must not
        // break a machine lacking the extension.
        [[nodiscard]] virtual auto IsBindlessSupported() const -> bool = 0;

        // Produce the shader-visible descriptor for one view and make it
        // resident. Returns 0 when the resource is dead or the view cannot be
        // realised; 0 is also the poison value, so a failure degrades to the
        // same deterministic wrong-render a use-after-free does rather than to
        // whatever bit pattern happened to be in the slot.
        //
        // RESIDENCY HAS NO VULKAN ANALOGUE and is the main way this rehearsal
        // differs from the real thing: `ARB_bindless_texture` requires an
        // explicit resident/non-resident transition per handle, and a texture is
        // immutable while any of its handles is resident. A descriptor-heap
        // Vulkan backend just writes the descriptor.
        [[nodiscard]] virtual auto AcquireDescriptor(ResourceHandle resource, const ViewDesc& view,
                                                     const SamplerDesc& sampler) -> u64 = 0;

        // Drop residency for a descriptor previously returned by
        // `AcquireDescriptor`. Called exactly once per successful acquire; the
        // backend refcounts internally, because two views that differ only in a
        // field the backend folds away legitimately produce the same descriptor.
        virtual void ReleaseDescriptor(u64 descriptor) = 0;

        // Publish a contiguous run of the CPU-side mirror to the GPU-visible
        // table. Called once per frame with the dirty range, not per slot.
        virtual void UploadSlots(u32 firstSlot, const u64* descriptors, u32 count) = 0;

        // Make the table reachable from shaders. Separate from `UploadSlots`
        // because the binding must be re-established on a frame where nothing
        // was dirty — and because on a descriptor-heap Vulkan backend this is a
        // command-buffer operation while the upload is not.
        virtual void BindHeap() = 0;
    };

    // -------------------------------------------------------------------------
    // The heap.
    //
    // Also the view registry: a view's entire reason to exist at this layer is
    // to own a slot, so fusing them removes a lookup and an opportunity for the
    // two to disagree. VIEW INDEX IS THE RESOURCE-HEAP SLOT INDEX (1:1), which
    // is what lets the frame-transient ring be a contiguous index range and
    // makes "the offset is stale" and "the view is dead" the same question.
    //
    // The SAMPLER heap is NOT 1:1, on purpose. ADR 0011 §1.2a: under
    // `VK_EXT_descriptor_heap` the sampler heap is a second, separate heap, and
    // the same sampler state used by 500 textures must occupy ONE sampler slot.
    // The engine has nothing to port into it — on OpenGL, filter and wrap state
    // live on the texture object, so there is no shareable sampler concept
    // anywhere. Sampler slots are therefore deduplicated by `SamplerDesc` value
    // and refcounted here, which is new engine machinery rather than converted
    // machinery.
    // -------------------------------------------------------------------------
    class DescriptorHeap
    {
      public:
        // One heap per process, matching `ResourceRegistry::Get()`. Leaked for
        // the same reason: a view can be destroyed during static destruction.
        [[nodiscard]] static auto Get() -> DescriptorHeap&;

        // `backend` may be null, which leaves the heap permanently disabled —
        // the state a headless test or an extension-less device runs in.
        // Re-initialising resets every slot and retires every outstanding view.
        void Initialize(const HeapDesc& desc, IDescriptorHeapBackend* backend);
        void Shutdown();

        // True when views can actually be created: initialised, a backend that
        // reports support, and the runtime toggle on. Every call site must be
        // able to take the other branch.
        [[nodiscard]] auto IsEnabled() const -> bool;

        // The runtime toggle. Defaults from the `OLO_RHI_BINDLESS` environment
        // variable at `Initialize`, and can be flipped live for A/B capture —
        // which is how a "the heap changed a pixel" claim gets tested against
        // the identical binary rather than against a rebuild.
        void SetEnabled(bool enabled);

        // ---------------------------------------------------------------------
        // Views
        // ---------------------------------------------------------------------

        // Allocate a slot, ask the backend for the descriptor, and mint the
        // identity that owns both. Returns a null handle when the heap is
        // disabled or out of slots — callers must treat that as "use the
        // slot-based path", never as a hard error, because the whole point of
        // the toggle is that both paths work.
        [[nodiscard]] auto CreateView(ResourceHandle resource, const ViewDesc& view, const SamplerDesc& sampler,
                                      HeapSlotLifetime lifetime) -> ViewHandle;

        // The form a render pass calls, once per texture it would have bound.
        //
        // For `Persistent` this MEMOISES: the same (resource, view, sampler)
        // triple returns the same `ViewHandle` for as long as the resource
        // lives. That is not an optimisation, it is required — a pass runs every
        // frame, and minting a fresh view per frame would drain the persistent
        // region in seconds while giving each frame a different offset, which is
        // precisely the stability ADR 0011 §1.2 calls "the performance argument
        // for bindless".
        //
        // For `FrameTransient` it always mints, because that is the whole point
        // of the ring: two acquisitions of one physical object in one frame must
        // get two offsets (see the aliasing note above).
        [[nodiscard]] auto GetOrCreateView(ResourceHandle resource, const ViewDesc& view, const SamplerDesc& sampler,
                                           HeapSlotLifetime lifetime) -> ViewHandle;

        // Persistent views only. A frame-transient view is retired wholesale by
        // `ResetFrameTransients` and destroying one by hand is a bug the heap counts
        // rather than honours.
        void DestroyView(ViewHandle view);

        // THE ONLY SANCTIONED WAY to turn a view into a shader-side index.
        // Validates the view's generation first; answers an invalid offset for a
        // stale, retired or foreign handle.
        [[nodiscard]] auto OffsetOf(ViewHandle view) const -> HeapOffset;

        // The sampler-heap index for the same view. Separate call rather than a
        // second return value because most shaders under a combined-sampler
        // model never need it, and a caller that does not use it should not be
        // paying for the lookup.
        [[nodiscard]] auto SamplerOffsetOf(ViewHandle view) const -> HeapOffset;

        [[nodiscard]] auto IsLive(ViewHandle view) const -> bool;
        [[nodiscard]] auto LifetimeOf(ViewHandle view) const -> HeapSlotLifetime;

        // ---------------------------------------------------------------------
        // Frame boundary and publication
        // ---------------------------------------------------------------------

        // Retire every frame-transient view and reset the ring cursor. Belongs
        // at exactly the moment `TransientPool::ReleaseAll()` runs, and for
        // exactly the same reason: that is when the physical objects the
        // transient descriptors named stop being owned by the passes that used
        // them.
        void ResetFrameTransients();

        // Push dirty slots to the GPU-visible table. Cheap and idempotent when
        // nothing changed, so calling it once per frame before execution is the
        // intended usage.
        void Flush();

        // ---------------------------------------------------------------------
        // Invalidation — the case the resource registry's design CREATES.
        //
        // `ResourceRegistry` deliberately PRESERVES a handle across an in-place
        // reload (`ScopedResourceHandle::Sync` repoints, never retires), which
        // is what makes caching a `ResourceHandle` safe. A descriptor does not
        // inherit that safety: under `ARB_bindless_texture` a texture handle
        // names the underlying object, and recreating the storage leaves every
        // descriptor for it naming a deleted object while the view's own
        // generation is unchanged — so `OffsetOf` cannot detect it.
        //
        // This is the exact mirror image of the Phase 2 slice-6 finding, where
        // stable identity made the redundant-bind cache SKIP a bind that had to
        // happen. Same root cause, opposite symptom: any site that recreates a
        // resource's storage must call this, exactly as it must call
        // `InvalidateTextureBinding`.
        // ---------------------------------------------------------------------
        void InvalidateResource(ResourceHandle resource);

        struct Stats
        {
            u32 PersistentCapacity = 0;
            u32 PersistentLive = 0;
            u32 TransientCapacity = 0;
            u32 TransientHighWater = 0; ///< peak ring use across the process
            u32 TransientThisFrame = 0; ///< ring slots handed out since the last reset
            u32 SamplerSlotsLive = 0;   ///< distinct SamplerDesc values, after dedup
            u64 ViewsCreated = 0;
            u64 StaleOffsetRejections = 0; ///< OffsetOf on a dead/foreign view
            u64 TransientOverflows = 0;    ///< ring exhausted; caller fell back
            u64 PersistentOverflows = 0;
            u64 DescriptorFailures = 0; ///< backend could not realise a view
            u64 SlotsPoisoned = 0;
        };
        [[nodiscard]] auto GetStats() const -> Stats;
        void ResetCounters();

        [[nodiscard]] auto IsPoisonOnFree() const -> bool;

      private:
        DescriptorHeap() = default;

        // The known-bad descriptor a freed slot is overwritten with when poison
        // is on. Zero rather than a sentinel bit pattern because that is what
        // both backends already treat as "nothing": a null `ARB_bindless_texture`
        // handle samples as zero instead of as the previous tenant, which turns
        // a use-after-free into a deterministic black read rather than a
        // plausible-looking wrong texture. Mirrors OLO_RG_POISON_TRANSIENTS,
        // which turned a stochastic aliasing artifact into a one-screenshot
        // signal.
        static constexpr u64 kPoisonDescriptor = 0u;

        struct ViewSlot
        {
            u32 Generation = 0u; ///< 0 = never handed out; `Handle::IsValid` rejects it
            bool Live = false;
            HeapSlotLifetime Lifetime = HeapSlotLifetime::Persistent;
            ResourceHandle Resource;
            ViewDesc View;
            u32 SamplerSlot = HeapOffset::Invalid;
            u64 Descriptor = kPoisonDescriptor;
        };

        // Caller must hold m_Mutex.
        [[nodiscard]] auto ValidateLocked(ViewHandle view) const -> const ViewSlot*;
        void ReleaseSlotLocked(u32 index);
        [[nodiscard]] auto AcquireSamplerSlotLocked(const SamplerDesc& sampler) -> u32;
        void ReleaseSamplerSlotLocked(u32 samplerSlot);
        void MarkDirtyLocked(u32 index);

        mutable std::mutex m_Mutex;

        bool m_Initialized = false;
        bool m_Enabled = false;
        HeapDesc m_Desc;
        IDescriptorHeapBackend* m_Backend = nullptr;

        // Slot layout: [0, m_PersistentCapacity) persistent, then the ring.
        // Fixed at Initialize and never resized, so a `ViewSlot`'s address is
        // stable and the vector never reallocates under a reader.
        std::vector<ViewSlot> m_Slots;
        std::vector<u64> m_Mirror; ///< CPU shadow of the GPU-visible table
        u32 m_PersistentCapacity = 0u;
        u32 m_TransientCapacity = 0u;
        std::vector<u32> m_PersistentFreeList;
        u32 m_TransientCursor = 0u;

        // Sampler heap: value-deduplicated, refcounted. `SamplerDesc` has a
        // defaulted `operator==` and is a small trivially-comparable value, so
        // a linear scan over the handful of distinct configurations an engine
        // actually uses beats hashing a struct with float members.
        struct SamplerSlot
        {
            SamplerDesc Desc;
            u32 RefCount = 0u;
        };
        std::vector<SamplerSlot> m_SamplerSlots;

        // Reverse index for InvalidateResource: resource slot index -> view
        // indices. Keyed on `ResourceHandle::Index` rather than the whole handle
        // so a reload (which preserves the handle) and a recreate (which does
        // not) both find their views.
        std::unordered_map<u32, std::vector<u32>> m_ViewsByResource;

        // Memoisation for GetOrCreateView's persistent case. The key carries the
        // resource's GENERATION as well as its index, so a destroy/recreate that
        // reuses the slot cannot hand the new resource the old resource's view —
        // the same reason the handle carries a generation at all.
        struct PersistentViewKey
        {
            u64 Resource = 0u; ///< RHI::HashKey(resource)
            u32 SamplerSlot = 0u;
            bool DepthCompare = true;

            [[nodiscard]] auto operator==(const PersistentViewKey& other) const -> bool = default;
        };
        struct PersistentViewKeyHash
        {
            [[nodiscard]] auto operator()(const PersistentViewKey& key) const noexcept -> std::size_t
            {
                return std::hash<u64>{}(key.Resource) ^ (std::hash<u32>{}(key.SamplerSlot) << 1u) ^
                       (key.DepthCompare ? 0x9E3779B9u : 0u);
            }
        };
        std::unordered_map<PersistentViewKey, ViewHandle, PersistentViewKeyHash> m_PersistentViewCache;

        u32 m_DirtyFirst = 0u;
        u32 m_DirtyLast = 0u; ///< exclusive; m_DirtyLast == m_DirtyFirst means clean

        // Mutable because the const query path COUNTS its own rejections. A
        // stale-offset rejection is the single most interesting thing this class
        // can observe — it is the moment a cached offset outlived its view — and
        // making `OffsetOf` non-const to record it would push the constness
        // problem out to every caller instead of solving it.
        mutable Stats m_Stats;
    };
} // namespace OloEngine::RHI

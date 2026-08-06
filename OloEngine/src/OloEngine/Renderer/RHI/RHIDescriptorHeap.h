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
    // The reserved null descriptor. Heap slot 0 is never handed out and stays
    // permanently poisoned, so an offset of 0 samples nothing on every backend.
    //
    // It exists because UNBIND DOES NOT SURVIVE THE TRANSLATION TO A HEAP. A
    // slot-based pass clears an input by binding a null texture; under the heap
    // there is no bind to clear — the shader reads an OFFSET, and leaving a stale
    // one in the table means it goes on sampling the previous texture through a
    // perfectly valid index. Every "I am not using this input" call site needs
    // somewhere honest to point, and this is it.
    inline constexpr u32 kNullHeapOffset = 0u;

    // The reserved null STORAGE-IMAGE descriptor (issue #691 Phase 3, bucket 3).
    //
    // A second reserved slot, and the reason generalises past this engine: a null
    // needs to exist PER DESCRIPTOR KIND, not per heap. Slot 0 holds a sampler
    // descriptor, and constructing an `image2D` from a sampler handle is
    // undefined behaviour just as surely as constructing one from zero is — so a
    // failed or cleared IMAGE binding pointing at `kNullHeapOffset` would swap a
    // stale-read bug for an undefined-behaviour bug rather than fixing anything.
    //
    // Same argument the null descriptor itself rests on, one axis over: the model
    // leans on poison reads being DETERMINISTIC, and determinism cannot come from
    // a descriptor the API says nothing about.
    inline constexpr u32 kNullStorageHeapOffset = 1u;

    // The reserved null CUBE / ARRAY / ARRAY-SHADOW descriptors, and they exist
    // because the argument above stops one level too shallow.
    //
    // "A null needs to exist per DESCRIPTOR KIND" was applied to sampler-vs-image
    // and left there — but a SAMPLER TYPE is a kind too. `samplerCube(h)` where
    // `h` names a 2D texture is undefined in exactly the way `image2D(h)` is, and
    // §5d (heap handles carry no type) says nothing stops a shader from doing it.
    // The null descriptor is then the one place the engine MANUFACTURES the
    // mismatch itself: every unset material and IBL lane resolves to
    // kNullHeapOffset, so a converted PBR shader with no environment probe built a
    // samplerCube from the 1x1 2D null on every draw.
    //
    // It read as an ORDER-DEPENDENT visible pop (issue #691 Phase 3): the rebase
    // evidence test passed alone and popped at boundaries 2 and 3 once a sibling
    // test had run first, because undefined behaviour is free to depend on
    // whatever the driver did previously. Four state-leak hypotheses died before
    // this one, all of them looking for something that was being written wrongly
    // rather than something being read as the wrong TYPE.
    inline constexpr u32 kNullCubeHeapOffset = 2u;
    inline constexpr u32 kNullArrayHeapOffset = 3u;
    inline constexpr u32 kNullArrayShadowHeapOffset = 4u;

    // The first slot the allocator may hand out. Every null sits below it.
    inline constexpr u32 kFirstAllocatableHeapSlot = kNullArrayShadowHeapOffset + 1u;

    // Which reserved null a given SAMPLER TYPE must fall back to. The shader knows
    // this and the C++ mostly does not — a TEX_* slot does not imply a type, since
    // TEX_USER_0..2 are generic slots that different shaders declare differently —
    // which is why the shader-side accessors in include/BindlessHeap.glsl do the
    // substitution and these constants are mirrored there.
    enum class NullSamplerKind : u8
    {
        Texture2D = 0,
        Cube,
        Texture2DArray,
        Texture2DArrayShadow,
    };

    [[nodiscard]] constexpr auto NullOffsetForSamplerKind(NullSamplerKind kind) -> u32
    {
        switch (kind)
        {
            case NullSamplerKind::Cube:
                return kNullCubeHeapOffset;
            case NullSamplerKind::Texture2DArray:
                return kNullArrayHeapOffset;
            case NullSamplerKind::Texture2DArrayShadow:
                return kNullArrayShadowHeapOffset;
            case NullSamplerKind::Texture2D:
            default:
                return kNullHeapOffset;
        }
    }

    // The null offset appropriate to a view's kind. Every "I am not using this
    // input" and every failed acquire must point at one of the two.
    [[nodiscard]] constexpr auto NullOffsetFor(ViewUsage usage) -> u32
    {
        return usage == ViewUsage::Storage ? kNullStorageHeapOffset : kNullHeapOffset;
    }

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
        // `view.Usage` selects the DESCRIPTOR KIND, and the two are genuinely
        // different API calls rather than two configurations of one
        // (`glGetTextureSamplerHandleARB` vs `glGetImageHandleARB`;
        // `COMBINED_IMAGE_SAMPLER` vs `STORAGE_IMAGE`). `sampler` is meaningless
        // for a storage view and backends must ignore it there.
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
        //
        // `usage` IS REQUIRED and is not a convenience: GL has two disjoint
        // residency namespaces with two entry-point pairs
        // (`glMakeTextureHandleNonResidentARB` vs
        // `glMakeImageHandleNonResidentARB`), and the spec does not promise that
        // a texture handle and an image handle are numerically distinguishable —
        // so a 64-bit value alone cannot say which call to make. Passing the kind
        // is cheaper and safer than a lookup that could answer wrongly.
        virtual void ReleaseDescriptor(u64 descriptor, ViewUsage usage) = 0;

        // Publish a contiguous run of the CPU-side mirror to the GPU-visible
        // table. Called once per frame with the dirty range, not per slot.
        virtual void UploadSlots(u32 firstSlot, const u64* descriptors, u32 count) = 0;

        // Make the table reachable from shaders. Separate from `UploadSlots`
        // because the binding must be re-established on a frame where nothing
        // was dirty — and because on a descriptor-heap Vulkan backend this is a
        // command-buffer operation while the upload is not.
        virtual void BindHeap() = 0;

        // The descriptor every unallocated, freed or cleared slot of this KIND
        // holds.
        //
        // MUST BE A REAL, VALID, SAMPLEABLE DESCRIPTOR — not a zero handle.
        // Sampling an invalid or non-resident `ARB_bindless_texture` handle is
        // UNDEFINED BEHAVIOUR, not a guaranteed black read, so a zeroed slot
        // would make poison-on-free and the null descriptor depend on driver
        // luck rather than on the spec. The GL backend returns the handle of a
        // resident 1x1 opaque-black texture; a Vulkan backend would return a
        // null-descriptor entry, which that API defines to read as zero.
        //
        // It takes a `usage` because a sampler handle and an image handle are not
        // interchangeable in a shader — `image2D(samplerHandle)` is undefined in
        // exactly the way `sampler2D(0)` is. One null per kind is the minimum
        // that keeps the determinism this model is built on.
        // `kind` is consulted only for ViewUsage::Sampled — a storage image has no
        // sampler type to mismatch.
        //
        // NO DEFAULT ARGUMENT, deliberately. A default on a VIRTUAL is bound to the
        // STATIC type of the expression, not the dynamic one, so a base and an
        // override that disagree about it silently change meaning with the pointer's
        // declared type — and this one was duplicated in both. Every caller now says
        // which null it wants, which is also the honest reading given that picking
        // the wrong kind is undefined behaviour rather than a wrong colour.
        [[nodiscard("the null descriptor is what a cleared binding samples — dropping it leaves the slot "
                    "undefined")]] virtual auto
        NullDescriptor(ViewUsage usage, NullSamplerKind kind) const -> u64 = 0;
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
        //
        // DISABLING IS NOT SYMMETRIC WITH ENABLING, and the asymmetry is a trap.
        // A shader's VARIANT is decided once, at compile time
        // (`OpenGLShader::WantsBindlessVariant` reads `IsEnabled()` and the
        // program is then cached). Turning the heap off afterwards does NOT turn
        // those programs back into slot-based ones — it just stops the binding
        // seam writing the offsets they still read. The result is a program
        // sampling a table nobody updates: a plausible, wrong, silent frame.
        //
        // Found the expensive way. `HeapGpuFixture::TearDown` calls `Shutdown()`,
        // and every later test in the suite whose shaders had already been built
        // bindless then rendered wrong. Tests that pass alone and fail in the
        // suite are the signature.
        //
        // Do NOT read a failure count into this. The same `Shutdown()` also bumps
        // the heap epoch, which used to strand the offset table on indices minted
        // by the previous heap (amendment (33)) — so the two defects overlapped in
        // the same tests, and an early attempt to size this one by counting
        // suite failures attributed the other one's damage to it. Excluding the
        // fixtures changed WHICH tests failed rather than how many, which is the
        // tell that a shared-state bug is in play and per-test attribution is
        // measuring the wrong thing.
        //
        // `AnyBindlessProgramsExist()` reports the hazard so a caller can decide;
        // the honest long-term fix is for a disable to force those programs to
        // reload as slot-based, which needs a shader-reload hook this layer does
        // not have.
        void SetEnabled(bool enabled);

        // True when at least one program was built as the bindless variant, i.e.
        // when disabling the heap would strand a program reading offsets nobody
        // writes. See SetEnabled.
        [[nodiscard]] static auto AnyBindlessProgramsExist() -> bool;

        // The backend this heap was initialised with, or null. Exists so a caller
        // that must Shutdown() and then restore the singleton — a test fixture
        // owning process-wide state — can bring it back up against the engine's
        // own backend instead of inventing one. See SetEnabled for why leaving it
        // down is not an option once any bindless program exists.
        [[nodiscard]] auto GetBackend() const -> IDescriptorHeapBackend*;

        // The descriptor this heap was initialised with. Pairs with GetBackend()
        // so a caller that must displace the singleton can put back the heap it
        // found — same capacities, not a plausible-looking guess that could
        // exhaust a ring the engine had sized larger.
        [[nodiscard]] auto GetDesc() const -> HeapDesc;

        // ---------------------------------------------------------------------
        // Views
        // ---------------------------------------------------------------------

        // Allocate a slot, ask the backend for the descriptor, and mint the
        // identity that owns both. Returns a null handle when the heap is
        // disabled or out of slots — callers must treat that as "use the
        // slot-based path", never as a hard error, because the whole point of
        // the toggle is that both paths work.
        [[nodiscard]] auto CreateView(ResourceHandle resource, const ViewDesc& view, const SamplerDesc& sampler,
                                      HeapSlotLifetime lifetime,
                                      NullSamplerKind kind = NullSamplerKind::Texture2D) -> ViewHandle;

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
                                           HeapSlotLifetime lifetime,
                                           NullSamplerKind kind = NullSamplerKind::Texture2D) -> ViewHandle;

        // ---------------------------------------------------------------------
        // Storage images — the SECOND descriptor kind (ADR 0011 amendment (26)).
        //
        // Separate entry points rather than a `SamplerDesc{}` argument on the two
        // above, because a storage view has no sampler at all and an API that
        // asks for one invites a caller to pass a meaningful-looking value that
        // is silently ignored. These force `Usage = ViewUsage::Storage` so a
        // caller cannot half-describe a storage view.
        //
        // Build `view` with `RHI::MakeStorageViewDesc(...)`, which takes the same
        // (mipLevel, layered, layer, access, format) the slot-based
        // `BindImageTexture` takes.
        //
        // The slot budget is shared with sampled views on purpose: under
        // `VK_EXT_descriptor_heap` a storage image is just another entry in the
        // same resource heap, and giving it a second allocator here would model a
        // split neither backend has. (The SAMPLER heap is genuinely separate —
        // §1.2a — and a storage view consumes none of it.)
        // ---------------------------------------------------------------------
        [[nodiscard]] auto CreateStorageView(ResourceHandle resource, const ViewDesc& view,
                                             HeapSlotLifetime lifetime) -> ViewHandle;
        [[nodiscard]] auto GetOrCreateStorageView(ResourceHandle resource, const ViewDesc& view,
                                                  HeapSlotLifetime lifetime) -> ViewHandle;

        // Which kind of descriptor a live view produces. Answers `Sampled` for a
        // dead handle, matching `LifetimeOf`'s reasoning: the offset is already
        // invalid, so nothing can act on this without first failing `OffsetOf`.
        [[nodiscard]] auto UsageOf(ViewHandle view) const -> ViewUsage;

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

        // ---------------------------------------------------------------------
        // DESTRUCTION IS NOT RE-CREATION, and conflating them kills the process.
        //
        // `InvalidateResource` above re-describes a view whose STORAGE changed
        // while the object lives on — it releases the old descriptor and
        // ACQUIRES A NEW ONE, which under `ARB_bindless_texture` makes the handle
        // RESIDENT AGAIN. That is right for a hot reload and catastrophic for a
        // delete: `glDeleteTextures` on a texture that still has a resident
        // handle is undefined, and in practice takes the driver — and the whole
        // process — down with no log line to show for it.
        //
        // Every framebuffer RESIZE deletes its attachments, so this is not an
        // exotic path. Call this before the GL object goes away; call
        // `InvalidateResource` only when the object survives and its storage did
        // not.
        //
        // Retires the views outright: residency dropped, slots poisoned,
        // generations advanced so any held `ViewHandle` reports stale through
        // `OffsetOf` rather than resolving into a slot that now belongs to
        // someone else.
        //
        // ADR 0011 amendment (22) says "every site that recreates a resource's
        // storage must call InvalidateResource" and does not draw this
        // distinction — that omission is what this pair exists to correct.
        // ---------------------------------------------------------------------
        void RetireResource(ResourceHandle resource);

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

        // Bumped by every Initialize/Shutdown. Anything that caches a GPU object
        // alongside the heap — the shared heap-offset UBO in RGCommandContext,
        // today — must compare this and rebuild when it moves.
        //
        // Not a nicety: that UBO is a function-local static, so it outlives a
        // device teardown and would otherwise be a dangling name from the
        // previous context. Found when the null-descriptor test passed alone and
        // failed in the full suite, where an earlier fixture had already brought
        // the renderer up and down — the same shape as a real context loss.
        [[nodiscard]] auto GetInitEpoch() const -> u64;

      private:
        DescriptorHeap() = default;

        // The descriptor a freed, cleared or never-allocated slot holds. Comes
        // from the BACKEND (`IDescriptorHeapBackend::NullDescriptor`), not from a
        // constant here.
        //
        // It used to be a hard-coded 0, on the reasoning that a null handle
        // "samples as zero". That is wrong: sampling an invalid or non-resident
        // ARB_bindless_texture handle is UNDEFINED BEHAVIOUR, so poison-on-free
        // and the null descriptor would have depended on driver luck rather than
        // on the spec — a deterministic instrument resting on undefined
        // behaviour is not an instrument. The backend supplies a real, resident,
        // sampleable black descriptor instead. Mirrors OLO_RG_POISON_TRANSIENTS,
        // which turned a stochastic aliasing artifact into a one-screenshot
        // signal; this keeps that property on solid ground.
        //
        // Per KIND: poisoning a released storage slot with a sampler handle would
        // put the shader back on undefined behaviour at exactly the moment the
        // instrument is supposed to be reporting.
        [[nodiscard]] auto PoisonDescriptorLocked(ViewUsage usage, NullSamplerKind kind) const -> u64;

        struct ViewSlot
        {
            u32 Generation = 0u; ///< 0 = never handed out; `Handle::IsValid` rejects it
            bool Live = false;
            HeapSlotLifetime Lifetime = HeapSlotLifetime::Persistent;
            ResourceHandle Resource;
            ViewDesc View;
            u32 SamplerSlot = HeapOffset::Invalid; ///< Invalid for a storage view — it consumes no sampler slot
            u64 Descriptor = 0u;                   ///< replaced by the backend's null descriptor on release
            /// The GLSL sampler type this view is read through. METADATA, and
            /// deliberately NOT a ViewDesc field: ViewDesc's defaulted operator== is
            /// the memoisation key, so putting the kind there would split one
            /// texture into two descriptors the moment two call sites disagreed.
            /// Kept here it costs nothing and answers the only question that needs
            /// it — which typed null to poison the slot with on release, so a
            /// samplerCube reader cannot be handed a 2D descriptor (issue #691).
            NullSamplerKind NullKind = NullSamplerKind::Texture2D;
        };

        // Caller must hold m_Mutex.
        [[nodiscard]] auto CreateViewLocked(ResourceHandle resource, const ViewDesc& view,
                                            const SamplerDesc& sampler, HeapSlotLifetime lifetime,
                                            NullSamplerKind kind) -> ViewHandle;
        [[nodiscard]] auto ValidateLocked(ViewHandle view) const -> const ViewSlot*;
        // Same check WITHOUT counting a rejection. For internal housekeeping —
        // evicting a stale cache entry is not a caller presenting a dead handle,
        // and folding the two together makes `StaleOffsetRejections` unusable for
        // the one thing the header says it is for: spotting the moment a cached
        // offset outlived its view.
        [[nodiscard]] auto IsSlotLiveLocked(ViewHandle view) const -> bool;
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
        //
        // IT ALSO CARRIES THE WHOLE `ViewDesc`, and that stopped being optional
        // when storage images arrived. The key used to be (resource, samplerSlot,
        // depthCompare) — sound only because the GL backend declined every view
        // that was not the whole resource, so no two distinguishable views could
        // reach the cache. A storage view is a mip level AND a format AND a layer
        // selection, and HZBGenerator alone asks for four levels of one texture in
        // one dispatch: under the old key the second request would have been
        // served the first's view and written every mip of the pyramid at level 0.
        // The `ViewDesc` compare is a handful of scalars and settles it by
        // construction rather than by enumerating which fields matter today.
        struct PersistentViewKey
        {
            u64 Resource = 0u; ///< RHI::HashKey(resource)
            u32 SamplerSlot = 0u;
            ViewDesc View;

            [[nodiscard]] auto operator==(const PersistentViewKey& other) const -> bool = default;
        };
        struct PersistentViewKeyHash
        {
            [[nodiscard]] auto operator()(const PersistentViewKey& key) const noexcept -> std::size_t
            {
                // Only the fields that actually vary across live views are mixed
                // in — a hash may collide, `operator==` is what decides identity,
                // and mixing every field would cost more than the linear probe it
                // saves.
                const std::size_t range = std::hash<u32>{}(key.View.Range.BaseMip) ^
                                          (std::hash<u32>{}(key.View.Range.BaseLayer) << 3u) ^
                                          (std::hash<u32>{}(key.View.Range.LayerCount) << 5u);
                return std::hash<u64>{}(key.Resource) ^ (std::hash<u32>{}(key.SamplerSlot) << 1u) ^
                       (key.View.DepthCompare ? 0x9E3779B9u : 0u) ^
                       (std::hash<u16>{}(static_cast<u16>(key.View.FormatOverride)) << 7u) ^
                       (std::hash<u8>{}(static_cast<u8>(key.View.Usage)) << 11u) ^
                       (std::hash<u8>{}(static_cast<u8>(key.View.StorageAccess)) << 13u) ^ range;
            }
        };
        std::unordered_map<PersistentViewKey, ViewHandle, PersistentViewKeyHash> m_PersistentViewCache;

        u64 m_InitEpoch = 0u;
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

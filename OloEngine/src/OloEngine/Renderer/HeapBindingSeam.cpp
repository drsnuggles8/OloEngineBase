#include "OloEnginePCH.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>

namespace OloEngine::HeapBinding
{
    namespace
    {
        // The shared heap-offset table (issue #691). One std140 UBO,
        // indexed by the very `TEX_*` constant — or, above HEAP_IMAGE_SLOT_BASE,
        // the very image unit — the caller used to bind with, so the bindless and
        // slot-based variants of a shader cannot disagree about which resource is
        // which.
        //
        // Function-local rather than a member of anything because the sites that
        // write it range from a render pass holding a per-frame context to a
        // process-lifetime compute system, while this buffer must outlive both.
        // Render passes and the compute systems both execute on the game thread,
        // so the unsynchronised scratch is safe; a `.Parallelizable()` caller
        // would need its own.
        struct HeapOffsetTable
        {
            // std140 pads a `uint` array to 16-byte stride, so the CPU side is
            // uvec4-shaped and the shader indexes [i >> 2][i & 3]. Getting this
            // wrong is the classic std140 trap: the array would read every fourth
            // offset and sample three wrong resources out of four.
            static constexpr u32 kSlots = ShaderBindingLayout::HEAP_OFFSET_TABLE_SLOTS;
            static constexpr u32 kVec4s = ShaderBindingLayout::HEAP_OFFSET_TABLE_VEC4S;

            std::array<u32, kVec4s * 4u> Scratch{};
            Ref<UniformBuffer> Buffer;
            bool Dirty = false;
            // The heap epoch this Buffer was created under. A function-local
            // static outlives a device teardown, so without this the buffer is a
            // dangling name from the previous context after any heap
            // re-initialisation — and the offsets silently stop arriving.
            u64 Epoch = 0u;
        };

        HeapOffsetTable& OffsetTable()
        {
            static HeapOffsetTable s_Table;
            return s_Table;
        }

        // Put every slot at its OWN kind's reserved null.
        //
        // NOT memset-to-zero, and the distinction is load-bearing: offset 0 holds a
        // SAMPLER descriptor, so zeroing the image region would hand `image2D` a
        // sampler handle — the exact undefined behaviour BindImageOrOffset's own
        // fallback goes out of its way to avoid. A default-constructed `Scratch{}`
        // has that defect at startup, which is why this also runs before the first
        // stage rather than only on a reset.
        void ResetScratchToNulls(HeapOffsetTable& table)
        {
            for (u32 i = 0u; i < HeapOffsetTable::kSlots; ++i)
            {
                table.Scratch[i] = (i < ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE) ? RHI::kNullHeapOffset
                                                                                   : RHI::kNullStorageHeapOffset;
            }
            table.Dirty = true;
        }

        // Re-base the table onto the heap currently in force.
        //
        // WHY A STALE SCRATCH IS WRONG RATHER THAN MERELY UNTIDY. Offsets are
        // indices into the heap that minted them. The heap bumps its epoch on every
        // Initialize/Shutdown, but this table is a process-lifetime static, so
        // across a re-initialisation every slot still holds an index minted against
        // the PREVIOUS heap. A pass re-stages the slots it binds and is therefore
        // fine; a slot it does not bind keeps a number that now addresses a
        // different descriptor entirely, and the bindless shader samples it without
        // complaint. Wrong pixels, no diagnostic, and the victim depends on which
        // pass ran last — which is why this presents as an order-dependent failure
        // across a test suite and never reproduces when a test is run alone.
        //
        // Called from StageOffset rather than only from FlushOffsets because a pass
        // stages before it flushes: resetting at flush time would wipe the very
        // offsets the pass just recorded.
        // The condition is the EPOCH ALONE. Do not add `|| !table.Buffer` to it:
        // this runs from StageOffset as well as FlushOffsets, and StageOffset
        // clears the buffer on a genuine reset — so a null buffer would make the
        // very next FlushOffsets "resync" and wipe the offsets the pass had just
        // staged between the two calls. That regression took out three heap GPU
        // tests before the suite caught it. Minting the buffer is FlushOffsets'
        // job and is keyed off `!table.Buffer` there, separately and on purpose.
        //
        // The process starts at Epoch 0 and `Initialize` bumps to at least 1, so
        // the first call always resets — which is what puts the image region on
        // its own null instead of the zero-initialised sampler null.
        void SyncEpoch(HeapOffsetTable& table)
        {
            const u64 epoch = RHI::DescriptorHeap::Get().GetInitEpoch();
            if (table.Epoch == epoch)
            {
                return;
            }

            ResetScratchToNulls(table);
            // The buffer is a GL name from the old context; drop it so the flush
            // mints a fresh one under the current heap.
            table.Buffer = nullptr;
            table.Epoch = epoch;
        }

        // BOTH conditions, and the second is not redundant. The heap's flag is
        // global; whether the program in flight reads the offset table is per
        // shader, because the bindless compile route is allowed to decline and
        // fall back to the ordinary slot-based program. Taking the heap branch for
        // such a program would record an offset, skip the bind, and leave its
        // sampler binding points empty — a pass rendering wrong with no diagnostic
        // at all.
        [[nodiscard]] bool HeapPathIsLive()
        {
            return RHI::DescriptorHeap::Get().IsEnabled() && Shader::IsBoundProgramBindless();
        }

        void StageOffset(u32 tableIndex, u32 value)
        {
            auto& table = OffsetTable();
            SyncEpoch(table);
            // AN IDENTICAL WRITE IS NOT A CHANGE, and the distinction is what makes
            // a per-draw flush affordable (issue #691). ADR 0011 amendment
            // (32) rejected converting the per-draw paths because "a converted
            // shader needs a flush per draw, which gives back exactly the cost
            // bindless exists to remove" — but the cost it names is the TABLE
            // UPLOAD, and consecutive draws in a bucket overwhelmingly restage the
            // same offsets (the same terrain textures across every patch, the same
            // atlas across every foliage layer). Guarding the dirty flag here turns
            // those flushes into a bool test plus the heap rebind that
            // DescriptorHeap::Flush owes anyway.
            //
            // SyncEpoch runs FIRST so this cannot swallow a re-base: it reseeds the
            // scratch with the nulls of the NEW heap, after which an incoming offset
            // differs and is written.
            if (table.Scratch[tableIndex] == value)
            {
                return;
            }
            table.Scratch[tableIndex] = value;
            table.Dirty = true;
        }
    } // namespace

    namespace
    {
        // The shared body of the two Resolve*TextureOffset entry points; they
        // differ only in their gate. Fetched at the point of USE, never stored
        // (ADR 0011 §1.2). A dead resource or an exhausted heap yields an
        // invalid offset, which tells the caller to keep binding through the
        // slot path.
        auto ResolveOffsetImpl(const RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler,
                               const RHI::HeapSlotLifetime lifetime, const RHI::NullSamplerKind kind)
            -> RHI::HeapOffset
        {
            RHI::ViewDesc viewDesc;
            viewDesc.Resource = texture;
            if (const RHI::ViewHandle view =
                    RHI::DescriptorHeap::Get().GetOrCreateView(texture, viewDesc, sampler, lifetime, kind);
                view.IsValid())
            {
                return RHI::OffsetOf(view);
            }
            return {};
        }
    } // namespace

    auto ResolveTextureOffset(const RHI::ResourceHandle texture, const RHI::HeapSlotLifetime lifetime,
                              const RHI::SamplerDesc& sampler, const RHI::NullSamplerKind kind) -> RHI::HeapOffset
    {
        if (!HeapPathIsLive())
        {
            return {};
        }
        return ResolveOffsetImpl(texture, sampler, lifetime, kind);
    }

    auto WritesOffsetsForBoundProgram() -> bool
    {
        return HeapPathIsLive();
    }

    auto ResolveRecordTextureOffset(const RHI::ResourceHandle texture, const RHI::SamplerDesc& sampler,
                                    const RHI::NullSamplerKind kind) -> RHI::HeapOffset
    {
        // Enablement alone, not the program in flight: a record is built for a
        // consumer that is not bound yet. Note the consequence on GL with the
        // heap enabled: every extracted material texture becomes resident, not
        // only the drawn ones, the same immutability the draw path imposes on
        // the textures it mints.
        if (!texture.IsValid() || !RHI::DescriptorHeap::Get().IsEnabled())
        {
            return {};
        }
        return ResolveOffsetImpl(texture, sampler, RHI::HeapSlotLifetime::Persistent, kind);
    }

    auto MaterialTexture2DSampler() -> const RHI::SamplerDesc&
    {
        static const RHI::SamplerDesc kSampler = []
        {
            RHI::SamplerDesc desc;
            desc.Source = RHI::SamplerSource::Explicit;
            desc.AddressU = RHI::AddressMode::Repeat;
            desc.AddressV = RHI::AddressMode::Repeat;
            desc.AddressW = RHI::AddressMode::Repeat;
            return desc;
        }();
        return kSampler;
    }

    namespace
    {
        // The shared body. `api` is null for the static form, in which case the
        // fallback goes through RenderCommand — see the header for why the
        // distinction has to survive down to the actual call.
        auto BindTextureOrOffsetImpl(RendererAPI* api, u32 slot, RHI::ResourceHandle texture,
                                     RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler,
                                     RHI::NullSamplerKind kind) -> RHI::HeapOffset;
    } // namespace

    auto BindTextureOrOffset(const u32 slot, const RHI::ResourceHandle texture,
                             const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler,
                             const RHI::NullSamplerKind kind) -> RHI::HeapOffset
    {
        return BindTextureOrOffsetImpl(nullptr, slot, texture, lifetime, sampler, kind);
    }

    auto BindTextureOrOffset(RendererAPI& api, const u32 slot, const RHI::ResourceHandle texture,
                             const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler,
                             const RHI::NullSamplerKind kind) -> RHI::HeapOffset
    {
        return BindTextureOrOffsetImpl(&api, slot, texture, lifetime, sampler, kind);
    }

    namespace
    {
        auto BindTextureOrOffsetImpl(RendererAPI* const api, const u32 slot, const RHI::ResourceHandle texture,
                                     const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler,
                                     const RHI::NullSamplerKind kind) -> RHI::HeapOffset
        {
            auto& heap = RHI::DescriptorHeap::Get();

            // A texture slot indexes the table directly — the image region sits above
            // MAX_ENGINE_TEXTURE_SLOTS, so a slot at or past it is a caller bug rather
            // than something to rebase.
            const bool slotInRange = slot < ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS;

            if (HeapPathIsLive() && slotInRange)
            {
                RHI::ViewDesc viewDesc;
                viewDesc.Resource = texture;
                // DERIVED, not defaulted, so the two knobs cannot disagree. The
                // backend already gates the GL compare mode on
                // `DepthCompare && sampler.Compare != Never`, so a caller asking
                // for a comparison sampler always means "compare", and one that
                // does not always means "give me the raw depth" — which is
                // exactly what the PCSS blocker search reads at
                // TEX_SHADOW_*_RAW. Leaving this at its `true` default made every
                // slot request a compare-capable view while the sampler said
                // Never, so the two raw views and the two comparison views of the
                // same depth array differed only by a field with no effect.
                viewDesc.DepthCompare = (sampler.Compare != RHI::CompareOp::Never);

                if (const RHI::ViewHandle view = heap.GetOrCreateView(texture, viewDesc, sampler, lifetime, kind);
                    view.IsValid())
                {
                    // Fetched at the point of write, never stored — ADR 0011 §1.2.
                    // For a persistent view this is a stable value the memoised lookup
                    // above already found; for a transient it is this frame's ring slot
                    // and is stale the moment the frame ends.
                    if (const RHI::HeapOffset offset = RHI::OffsetOf(view); offset.IsValid())
                    {
                        StageOffset(slot, offset.Value);
                        return offset;
                    }
                }
            }

            // Every failure lands here, and that is the design: a machine without the
            // extension, a heap that filled up, a resource that died mid-frame, and a
            // slot outside the table all render the frame the old way rather than
            // rendering it wrong.
            //
            // Through the CALLER'S api when it has one — the dispatch handlers are
            // executed against MockRendererAPI in the test suite, and the static
            // facade would bypass it invisibly.
            //
            // The DESC travels with the bind (#691): on GL the slot
            // path samples with the texture object's state and the default
            // three-arg body reduces to the old two-arg bind — but the Vulkan
            // backend's samplers are heap objects, and dropping the desc here
            // was exactly how an explicit compare/filter request (the
            // ShadowDepthSampler state, SSAO's Nearest/Repeat noise) silently
            // degraded to the inherit state on that backend.
            if (api != nullptr)
            {
                api->BindTexture(slot, texture, sampler);
            }
            else
            {
                RenderCommand::BindTexture(slot, texture, sampler);
            }

            // …but when the heap is ON, falling back is not enough: the shader is the
            // bindless variant and reads the OFFSET, not the binding. A stale entry
            // left in the table would keep sampling whatever this slot pointed at last
            // — including across an intentional unbind, which is how a pass says "I am
            // not using this input this frame" (ToneMapRenderPass binds
            // RHI::NullResource for exactly that). Point it at the reserved null
            // descriptor so the shader samples nothing instead.
            if (HeapPathIsLive() && slotInRange)
            {
                StageOffset(slot, RHI::NullOffsetForSamplerKind(kind));
            }
            return {};
        }

    } // namespace

    auto ShadowDepthSampler(const bool comparison) -> RHI::SamplerDesc
    {
        // Every field here is READ OFF THE BACKEND rather than chosen, because the
        // claim this phase makes is that only the binding MECHANISM changes.
        // OpenGLTexture2DArray gives a DEPTH_COMPONENT32F array CLAMP_TO_BORDER on
        // all three axes with an opaque-WHITE border, so a lookup outside a cascade
        // reads "lit" instead of "fully shadowed"; and, when DepthComparisonMode is
        // set, COMPARE_REF_TO_TEXTURE with LEQUAL.
        RHI::SamplerDesc desc;
        desc.Source = RHI::SamplerSource::Explicit;
        // EXPLICIT, or the fields below are ignored and the descriptor inherits the
        // texture object instead — which for the raw view would leave comparison ON.
        desc.AddressU = RHI::AddressMode::ClampToBorder;
        desc.AddressV = RHI::AddressMode::ClampToBorder;
        desc.AddressW = RHI::AddressMode::ClampToBorder;
        desc.Border = RHI::BorderColor::OpaqueWhite;
        desc.Compare = comparison ? RHI::CompareOp::LessOrEqual : RHI::CompareOp::Never;

        // NO MIP FILTERING: the arrays are created with one level, and the
        // SamplerDesc default (LinearMipFilter = true) resolves to
        // GL_LINEAR_MIPMAP_LINEAR, which makes a single-level texture INCOMPLETE.
        // "Read the sampler state off the backend" means all of it — the same
        // omission the heap backend's ToGLMinFilter note records for SSAO's noise.
        desc.LinearMipFilter = false;
        return desc;
    }

    auto PublishTextureOffsetAndBind(const u32 slot, const RHI::ResourceHandle texture,
                                     const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler,
                                     const RHI::NullSamplerKind kind)
        -> RHI::HeapOffset
    {
        auto& heap = RHI::DescriptorHeap::Get();
        const bool slotInRange = slot < ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS;

        RHI::HeapOffset published;

        // NOT gated on Shader::IsBoundProgramBindless() — see the header. At
        // publish time the consuming program is not in flight, so that flag
        // answers a question about an unrelated shader. The heap's own toggle is
        // the only condition that means anything here.
        if (heap.IsEnabled() && slotInRange)
        {
            RHI::ViewDesc viewDesc;
            viewDesc.Resource = texture;

            if (const RHI::ViewHandle view = heap.GetOrCreateView(texture, viewDesc, sampler, lifetime, kind);
                view.IsValid())
            {
                if (const RHI::HeapOffset offset = RHI::OffsetOf(view); offset.IsValid())
                {
                    StageOffset(slot, offset.Value);
                    published = offset;
                }
            }

            if (!published.IsValid())
            {
                // The heap is on and a bindless consumer WILL read this slot, so
                // leaving the previous frame's offset there is the stale-read
                // hazard. Point it at the reserved null instead.
                StageOffset(slot, RHI::NullOffsetForSamplerKind(kind));
            }
        }

        // ALWAYS bind as well. A slot-based consumer of this same slot reads the
        // BINDING, not the offset, and a slot declared in a shared include/ header
        // is guaranteed to have one whenever any includer stays slot-based —
        // TEX_WIND_FIELD's Particle_Simulate.comp today. This bind is what keeps
        // those working, and it is the mechanism BindlessShaderPipeline's
        // SlotAlwaysReceivesARealBind allowlist is allowed to point at.
        // The DESC travels with the bind (the BindTextureOrOffsetImpl rule):
        // dropping it here degraded an explicit compare/filter request to the
        // inherit state on the Vulkan backend, where samplers are heap objects.
        RenderCommand::BindTexture(slot, texture, sampler);

        return published;
    }

    auto BindImageOrOffset(const u32 imageUnit, const RHI::ResourceHandle texture, const u32 mipLevel,
                           const bool layered, const u32 layer, const RHI::Access access,
                           const RHI::Format format, const RHI::HeapSlotLifetime lifetime) -> RHI::HeapOffset
    {
        auto& heap = RHI::DescriptorHeap::Get();

        const bool unitInRange = imageUnit < ShaderBindingLayout::MAX_ENGINE_IMAGE_SLOTS;
        const u32 tableIndex = ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE + imageUnit;

        if (HeapPathIsLive() && unitInRange)
        {
            const RHI::ViewDesc viewDesc =
                RHI::MakeStorageViewDesc(texture, mipLevel, layered, layer, access, format);

            if (const RHI::ViewHandle view = heap.GetOrCreateStorageView(texture, viewDesc, lifetime);
                view.IsValid())
            {
                if (const RHI::HeapOffset offset = RHI::OffsetOf(view); offset.IsValid())
                {
                    StageOffset(tableIndex, offset.Value);
                    return offset;
                }
            }
        }

        RenderCommand::BindImageTexture(imageUnit, texture, mipLevel, layered, layer, access, format);

        // The image kind's own reserved null, NOT kNullHeapOffset. Slot 0 holds a
        // SAMPLER descriptor, and constructing an `image2D` from a sampler handle
        // is undefined in exactly the way constructing one from zero is — so
        // pointing a cleared image binding there would swap a stale-read bug for an
        // undefined-behaviour one instead of fixing it.
        if (HeapPathIsLive() && unitInRange)
        {
            // ITS OWN FORMAT'S null, not the shared R32F one. An image handle bakes
            // the format in, and loading through a disagreeing `layout(...)`
            // qualifier is undefined rather than a reinterpretation — so an r32ui
            // or rgba32f binding cleared to the R32F null was undefined on the
            // ordinary path (issue #691).
            StageOffset(tableIndex, RHI::NullOffsetForStorageFormat(format));
        }
        return {};
    }

    void FlushOffsets()
    {
        auto& table = OffsetTable();
        if (!RHI::DescriptorHeap::Get().IsEnabled())
        {
            return;
        }

        // PUBLISH THE DESCRIPTORS FIRST, and this ordering is the whole reason
        // this call exists rather than a once-per-frame publish.
        //
        // A pass mints its views inside its own Execute — the bind calls above are
        // pass-time calls, not planning-time ones. So the frame-level
        // `DescriptorHeap::Flush()` in `RenderGraph::Execute` runs BEFORE any of
        // this frame's transient descriptors exist: they land in the CPU mirror,
        // get marked dirty, and are never uploaded. The offsets would then index a
        // heap whose slots still hold the previous frame's (or no) descriptor.
        //
        // Found by converting a batch of post-process passes and getting a black
        // viewport. It hid until then for two reasons worth remembering: the first
        // converted pass was one the active render path did not execute, and the
        // GPU test called `DescriptorHeap::Flush()` by hand right after this
        // function — so the test was supplying the very call the engine was
        // missing. A test that sequences a mechanism for itself cannot detect that
        // the engine fails to sequence it.
        RHI::DescriptorHeap::Get().Flush();

        // BEFORE the dirty check, not after. A pass that stages nothing still needs
        // the table re-based across a heap re-initialisation — and the old code
        // returned early here, so the buffer was never even recreated in that case.
        SyncEpoch(table);

        if (!table.Dirty)
        {
            return;
        }

        if (!table.Buffer)
        {
            table.Buffer = UniformBuffer::Create(static_cast<u32>(table.Scratch.size() * sizeof(u32)),
                                                 ShaderBindingLayout::UBO_HEAP_OFFSETS);
        }

        table.Buffer->SetData(table.Scratch.data(), static_cast<u32>(table.Scratch.size() * sizeof(u32)));
        table.Dirty = false;
    }

    auto StagedOffsetAt(const u32 tableIndex) -> RHI::HeapOffset
    {
        if (tableIndex >= HeapOffsetTable::kSlots)
        {
            return {};
        }
        return RHI::HeapOffset{ OffsetTable().Scratch[tableIndex] };
    }
} // namespace OloEngine::HeapBinding

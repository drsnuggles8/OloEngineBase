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
        // The shared heap-offset table (issue #691 Phase 3). One std140 UBO,
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
            table.Scratch[tableIndex] = value;
            table.Dirty = true;
        }
    } // namespace

    auto WritesOffsetsForBoundProgram() -> bool
    {
        return HeapPathIsLive();
    }

    namespace
    {
        // The shared body. `api` is null for the static form, in which case the
        // fallback goes through RenderCommand — see the header for why the
        // distinction has to survive down to the actual call.
        auto BindTextureOrOffsetImpl(RendererAPI* api, u32 slot, RHI::ResourceHandle texture,
                                     RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler)
            -> RHI::HeapOffset;
    } // namespace

    auto BindTextureOrOffset(const u32 slot, const RHI::ResourceHandle texture,
                             const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler)
        -> RHI::HeapOffset
    {
        return BindTextureOrOffsetImpl(nullptr, slot, texture, lifetime, sampler);
    }

    auto BindTextureOrOffset(RendererAPI& api, const u32 slot, const RHI::ResourceHandle texture,
                             const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler)
        -> RHI::HeapOffset
    {
        return BindTextureOrOffsetImpl(&api, slot, texture, lifetime, sampler);
    }

    namespace
    {
        auto BindTextureOrOffsetImpl(RendererAPI* const api, const u32 slot, const RHI::ResourceHandle texture,
                                     const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler)
            -> RHI::HeapOffset
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

                if (const RHI::ViewHandle view = heap.GetOrCreateView(texture, viewDesc, sampler, lifetime);
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
            if (api != nullptr)
            {
                api->BindTexture(slot, texture);
            }
            else
            {
                RenderCommand::BindTexture(slot, texture);
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
                StageOffset(slot, RHI::kNullHeapOffset);
            }
            return {};
        }

    } // namespace

    auto PublishTextureOffsetAndBind(const u32 slot, const RHI::ResourceHandle texture,
                                     const RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler)
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

            if (const RHI::ViewHandle view = heap.GetOrCreateView(texture, viewDesc, sampler, lifetime);
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
                StageOffset(slot, RHI::kNullHeapOffset);
            }
        }

        // ALWAYS bind as well. A slot-based consumer of this same slot — of which
        // there is at least one for the DDGI atlases (Skybox_GBuffer) — reads the
        // binding, not the offset, and cannot be converted while the bindless
        // route produces no reflection.
        RenderCommand::BindTexture(slot, texture);

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
            StageOffset(tableIndex, RHI::kNullStorageHeapOffset);
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

        if (!table.Dirty)
        {
            return;
        }

        // Rebuild across a device teardown. The heap bumps its epoch on every
        // Initialize/Shutdown, and this buffer is a process-lifetime static, so
        // without the comparison it would keep writing into a GL name that belonged
        // to the previous context — offsets that silently never arrive.
        if (const u64 epoch = RHI::DescriptorHeap::Get().GetInitEpoch(); !table.Buffer || table.Epoch != epoch)
        {
            table.Buffer = UniformBuffer::Create(static_cast<u32>(table.Scratch.size() * sizeof(u32)),
                                                 ShaderBindingLayout::UBO_HEAP_OFFSETS);
            table.Epoch = epoch;
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

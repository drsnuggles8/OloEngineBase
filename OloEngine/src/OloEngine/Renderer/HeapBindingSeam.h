#pragma once

// =============================================================================
// HeapBindingSeam.h — the one place a bind forks into "write an offset" or
// "bind the old way".
//
// Issue #691 Phase 3, ADR 0011 §1.1 / amendments (25) and (26).
//
// WHY THIS IS NOT A MEMBER OF RGCommandContext, where the sampler half started.
// Most storage-image bindings in this engine are issued by COMPUTE SYSTEMS that
// have no render-graph context at all — SnowAccumulationSystem, WindSystem,
// CloudNoise, CloudShadowMap, HZBGenerator, OceanFFTGpu, TerrainErosion — and
// they reach the driver through the static `RenderCommand` facade. A seam only a
// pass could call would have left ~60% of the image sites unconvertible, or
// forced a second offset table for the ones that could not reach the first.
//
// There must be exactly ONE table. It is indexed by slot number, and two tables
// would mean two flushes, two binding points, and two chances for a pass and its
// shader to disagree about which one carries a given slot.
//
// `RGCommandContext` keeps its `BindTextureOrHeapOffset` / `FlushHeapOffsets`
// spelling and forwards here, so no already-converted pass changes.
//
// THE TABLE'S INDEX SPACE, and the one thing to get right when adding a kind:
// texture slots occupy [0, MAX_ENGINE_TEXTURE_SLOTS) and image units occupy
// [HEAP_IMAGE_SLOT_BASE, + MAX_ENGINE_IMAGE_SLOTS). GL image units and texture
// units are separate namespaces that both start at zero, so without the base a
// compute pass writing image unit 0 would overwrite TEX_DIFFUSE's offset and
// each would silently render the other's resource.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

namespace OloEngine
{
    class RendererAPI;
}

namespace OloEngine::HeapBinding
{
    // The heap-bindless form of `RenderCommand::BindTexture`.
    //
    //   * heap enabled and the bound program is the bindless variant -> mints or
    //     looks up the view, records its offset at table index `slot`, binds
    //     NOTHING.
    //   * anything else (no extension, toggle off, heap full, dead resource,
    //     slot out of range) -> falls back to `RenderCommand::BindTexture`.
    //
    // `slot` keeps its meaning either way: a converted shader indexes the table
    // with the same `TEX_*` constant it used to declare `layout(binding = N)`
    // with, so the two variants cannot disagree about which texture is which.
    auto BindTextureOrOffset(u32 slot, RHI::ResourceHandle texture, RHI::HeapSlotLifetime lifetime,
                             const RHI::SamplerDesc& sampler = {}) -> RHI::HeapOffset;

    // The same seam for a caller that already holds a `RendererAPI&`, i.e. the
    // command-bucket dispatch handlers.
    //
    // THE OVERLOAD EXISTS FOR THE FALLBACK, and it is not a convenience. The
    // handlers receive their `api` by reference and the test suite dispatches
    // packets against `MockRendererAPI`, which records every `BindTexture` — so
    // routing the fallback through the static `RenderCommand` facade instead
    // would keep compiling, keep working in the editor, and silently stop the
    // mock from ever seeing the call. That is the same shape as the `.data()`
    // trap Phase 2 recorded: a change the type system cannot object to that
    // redirects which object is actually spoken to.
    //
    // The heap path is identical either way — a heap write touches no backend.
    auto BindTextureOrOffset(RendererAPI& api, u32 slot, RHI::ResourceHandle texture,
                             RHI::HeapSlotLifetime lifetime, const RHI::SamplerDesc& sampler = {})
        -> RHI::HeapOffset;

    // PUBLISHED-GLOBAL bindings: write the offset AND perform the bind.
    //
    // `BindTextureOrOffset` above forks on `Shader::IsBoundProgramBindless()`,
    // which describes the program CURRENTLY in flight. That is the right question
    // for a pass binding its own inputs immediately before its own draw, and a
    // meaningless one for a binding published for LATER passes to consume — at
    // publish time the consuming program is not bound, and often has not been
    // created yet. `SetGlobalIBL`, the DDGI atlases, the shadow maps, the froxel
    // fog volume and the cloud-shadow map are all this shape
    // (docs/agent-rules/render-pass-published-state.md).
    //
    // A published slot can also have consumers of BOTH kinds at once, so there is
    // no single right answer to fork to: TEX_DDGI_IRRADIANCE is read by the DDGI
    // shaders and DeferredLighting (convertible) *and* by Skybox_GBuffer, which
    // cannot take the bindless route at all because that route produces no SPIR-V
    // and so never runs Reflect().
    //
    // Doing BOTH is therefore not belt-and-braces, it is the only correct answer
    // while a slot has mixed consumers: a bindless consumer reads the offset, a
    // slot-based one reads the binding, and neither can tell the other happened.
    // The cost is one redundant bind on a PER-PASS path — not per draw — which is
    // the same trade the redundant-bind cache already makes everywhere else.
    //
    // Prefer `BindTextureOrOffset` whenever the consuming shader is bound at the
    // call site; reach for this only for genuinely published state.
    auto PublishTextureOffsetAndBind(u32 slot, RHI::ResourceHandle texture, RHI::HeapSlotLifetime lifetime,
                                     const RHI::SamplerDesc& sampler = {}) -> RHI::HeapOffset;

    // THE SAMPLER STATE A SHADOW-MAP DESCRIPTOR MUST BE MINTED WITH, in one place
    // because it has to be identical at every site that stages one.
    //
    // A slot bind samples with the TEXTURE's parameters, so every consumer of a
    // shadow map got its comparison mode for free no matter who bound it. A heap
    // descriptor samples with the SAMPLER OBJECT baked into the handle, so the
    // state has to be supplied — and the seam's default SamplerDesc{} carries
    // `Compare = Never`, which the backend turns into GL_TEXTURE_COMPARE_MODE =
    // GL_NONE. Reading such a handle as `sampler2DArrayShadow` is UNDEFINED, and
    // in practice reads as "unshadowed": shadows LEAK rather than disappear,
    // which is far harder to notice.
    //
    // WORSE, IT IS A LAST-WRITER RACE ACROSS PASSES. Several passes stage an
    // offset for TEX_SHADOW / TEX_SHADOW_ATLAS, and whichever ran last owns the
    // published descriptor for every bindless reader that frame. One site
    // defaulting the sampler therefore silently disables comparison for shaders
    // that never asked it to — which is exactly how converting PBR_MultiLight's
    // shadow samplers made DDGI's lit/dark bands collapse from 60/33 to 62/41
    // (issue #691 Phase 3).
    //
    // `comparison == false` gives the raw-depth view the PCSS blocker search
    // reads; the seam derives ViewDesc::DepthCompare from the Compare field, so
    // the two views of one depth array cannot drift apart.
    [[nodiscard]] auto ShadowDepthSampler(bool comparison) -> RHI::SamplerDesc;

    // Resolve a texture to a heap offset WITHOUT staging it in the shared table.
    //
    // FOR PER-MATERIAL OFFSETS, which are the reason the shared table is not the
    // whole answer. The table is indexed by `TEX_*` slot and published by
    // `FlushOffsets()`; a MATERIAL's textures change per draw, so routing them
    // through it would mean re-uploading the whole table for every draw — the
    // per-draw cost bindless exists to remove. Their offsets instead ride in the
    // material UBO, which the draw path already uploads per material, so carrying
    // nine more integers there costs nothing extra (ADR 0011 §1.2, amendment (32)).
    //
    // Returns an invalid offset when the heap path is not live for the program in
    // flight, so the caller keeps binding the old way — the same fork every other
    // entry point here makes, just without the write.
    [[nodiscard("the resolved offset is the only way the shader finds the texture")]] auto ResolveTextureOffset(RHI::ResourceHandle texture, RHI::HeapSlotLifetime lifetime,
                                                                                                                const RHI::SamplerDesc& sampler = {}) -> RHI::HeapOffset;

    // True when the program in flight reads the offset table, i.e. when a bind
    // through this seam records an offset instead of touching the driver.
    //
    // EXPOSED FOR ONE REASON: a caller with its own redundant-bind cache must not
    // let that cache short-circuit the offset write. The two caches guard
    // different things — "this slot's GL binding is already correct" is not the
    // same claim as "this slot's OFFSET is already correct", because the offset
    // table is shared with every pass that binds through the seam while
    // `CommandDispatch::BoundTextures` only tracks binds that went through
    // CommandDispatch. Under the heap the write is a CPU array store, so
    // skipping it saves nothing and can leave another pass's offset in place.
    //
    // Do NOT use this to fork behaviour that the seam already forks for you.
    [[nodiscard]] auto WritesOffsetsForBoundProgram() -> bool;

    // The heap-bindless form of `RenderCommand::BindImageTexture`, taking exactly
    // the parameters that call already takes so a conversion is a spelling change.
    //
    // `imageUnit` is a GL IMAGE unit, not a texture slot — it is rebased onto the
    // table's image region here, and `include/BindlessHeap.glsl`'s
    // OLO_HEAP_IMAGE_* macros apply the identical base on the shader side from the
    // same constant.
    //
    // `format` is MANDATORY (`Format::Unknown` falls back): a storage image's
    // format is part of its binding contract and must match the shader's format
    // layout qualifier, so there is nothing sensible to infer.
    auto BindImageOrOffset(u32 imageUnit, RHI::ResourceHandle texture, u32 mipLevel, bool layered, u32 layer,
                           RHI::Access access, RHI::Format format, RHI::HeapSlotLifetime lifetime)
        -> RHI::HeapOffset;

    // Publish the descriptors minted since the last flush AND the offsets that
    // index them, in that order. Call once before the draw or dispatch.
    //
    // BOTH, and the order is the whole reason this is not a once-per-frame
    // publish: a pass mints its views inside its own Execute, so a frame-level
    // `DescriptorHeap::Flush()` runs before this frame's transient descriptors
    // exist — they land in the CPU mirror, get marked dirty and are never
    // uploaded, and the offsets then index slots holding the previous frame's
    // descriptor. That rendered a black viewport for a whole batch of
    // conversions.
    void FlushOffsets();

    // Diagnostics/tests only: the value currently staged at a table index, or
    // HeapOffset::Invalid for an out-of-range index. Reads the CPU scratch, not
    // the GPU buffer, so it answers "what would the next flush publish".
    [[nodiscard]] auto StagedOffsetAt(u32 tableIndex) -> RHI::HeapOffset;
} // namespace OloEngine::HeapBinding

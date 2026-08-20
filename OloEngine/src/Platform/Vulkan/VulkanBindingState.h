#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanBindingState — the process-global bind-point mirror (#691 Phase 7).
//
// GL's binding model is PROCESS-GLOBAL state: glBindBufferBase points,
// texture units, the current program and the bound framebuffer all live on
// the context, and every pass assumes exactly that. The Vulkan draw path
// keeps those semantics by mirroring them here: resource objects PUBLISH
// (VulkanUniformBuffer::Bind / ctor, VulkanShader::Bind,
// VulkanFramebuffer::Bind, VulkanRendererAPI::BindTexture), and the draw
// assembly READS at record time to build the root-data struct (ADR 0011 §4)
// and the dynamic-rendering scope.
//
// Deliberately a singleton rather than VulkanRendererAPI instance state: the
// publishers are resource classes that cannot (and should not) know which
// API instance is active, and GL's own semantics are global — a test-local
// VulkanRendererAPI sharing this state with the process is FAITHFUL, not a
// leak. (This also dodges the injected-vs-global instance trap
// CommandDispatch's bind-cache helpers document.)
//
// Dangling-pointer discipline: every publisher NULLS its own entry on
// destruction (ClearIfCurrent / ClearBuffer). A stale entry is therefore
// impossible by construction, matching GL's delete-implies-unbind.
//
// Thread-safety: NONE, deliberately — render thread only.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>

namespace OloEngine
{
    class VulkanUniformBuffer;
    class VulkanStorageBuffer;
    class VulkanShader;
    class VulkanFramebuffer;

    class VulkanBindingState
    {
      public:
        [[nodiscard]] static VulkanBindingState& Get();

        // These bound the mirror arrays, so a binding at or above them is
        // DROPPED (with a warn) and its later lookup answers null — the shader
        // then reads a zero address or the reserved null descriptor and
        // renders wrong, not loudly. They must therefore stay above every
        // number ShaderBindingLayout can hand out, which the static_asserts
        // below enforce at compile time rather than at first sight of a black
        // frame (#691 Phase 7: the auto-exposure block moving to 72 on the
        // #705 merge, and the A2 renumber pushing TEX_DDGI_VISIBILITY to 64
        // and TEX_SHADER_GRAPH_0 to 65, both walked past the old 64).
        //
        // kMaxBufferBindings ALIASES the backend-neutral constant rather than repeating the
        // literal: the bound is GL_MAX_UNIFORM_BUFFER_BINDINGS' minimum guarantee, a property of
        // the binding NUMBERS the engine hands out and not of Vulkan. Defining it in
        // ShaderBindingLayout.h is what lets a GL-only TU assert against it without reaching into
        // this header, which vanishes entirely under OLO_WITH_VULKAN=OFF — VirtualShadowMapLocalTest
        // did exactly that and had stopped compiling in that configuration (#811).
        static constexpr u32 kMaxBufferBindings = ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS;
        static constexpr u32 kMaxTextureSlots = 96; // engine slots + shader-graph user slots, with headroom
        static constexpr u32 kNoHeapSlot = 0xFFFFFFFFu;

        static_assert(ShaderBindingLayout::UBO_BINDING_LIMIT <= kMaxBufferBindings,
                      "every engine UBO binding must fit the bind-state mirror");
        static_assert(ShaderBindingLayout::SSBO_BONE_PULL < kMaxBufferBindings,
                      "the highest engine SSBO binding must fit the bind-state mirror");
        static_assert(ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS <= kMaxTextureSlots,
                      "every engine texture slot must fit the bind-state mirror");

        // --- buffer bind points (glBindBufferBase mirror) --------------------
        void SetUniformBuffer(u32 binding, VulkanUniformBuffer* buffer);
        void SetStorageBuffer(u32 binding, VulkanStorageBuffer* buffer);
        [[nodiscard]] VulkanUniformBuffer* GetUniformBuffer(u32 binding) const;
        [[nodiscard]] VulkanStorageBuffer* GetStorageBuffer(u32 binding) const;
        // Called from destructors: drop every entry pointing at `buffer`.
        void ClearBuffer(const void* buffer);

        // --- texture slots (heap slot indices staged by BindTexture) ---------
        void SetTextureHeapSlot(u32 slot, u32 heapSlot);
        [[nodiscard]] u32 GetTextureHeapSlot(u32 slot) const;

        // --- sampler slots (#691 Phase 8) ------------------------------------
        // The SAMPLER-heap slot staged beside each texture slot: BindTexture
        // derives it from the image's recorded sampler state (or an explicit
        // RHI::SamplerDesc) and the draw assembly writes it into the root
        // struct's sampler-index half. An unstaged slot answers
        // VulkanSamplerHeap::DefaultSlot (0), which is the old embedded
        // linear/clamp default — never a garbage index.
        void SetTextureSamplerSlot(u32 slot, u32 samplerSlot);
        [[nodiscard]] u32 GetTextureSamplerSlot(u32 slot) const;

        // --- image units (BindImageTexture) — a SEPARATE index space ---------
        // GL's image units and texture units are disjoint namespaces that both
        // start at zero (amendment (29)); folding them here would let unit 0
        // and TEX_DIFFUSE publish over each other — a wrong REAL resource,
        // the model's worst failure shape.
        void SetImageHeapSlot(u32 unit, u32 heapSlot);
        [[nodiscard]] u32 GetImageHeapSlot(u32 unit) const;

        // --- current render target -------------------------------------------
        // (The current SHADER deliberately lives on VulkanShader itself —
        // s_CurrentlyBound / GetCurrentlyBound(), Phase 6's seam. One source
        // of truth; this class does not duplicate it.)
        void SetCurrentFramebuffer(VulkanFramebuffer* framebuffer);
        [[nodiscard]] VulkanFramebuffer* GetCurrentFramebuffer() const;
        // Called from destructors.
        void ClearIfCurrentFramebuffer(const VulkanFramebuffer* framebuffer);

      private:
        VulkanBindingState() = default;

        std::array<VulkanUniformBuffer*, kMaxBufferBindings> m_UniformBuffers{};
        std::array<VulkanStorageBuffer*, kMaxBufferBindings> m_StorageBuffers{};
        std::array<u32, kMaxTextureSlots> m_TextureHeapSlots{};
        std::array<u32, kMaxTextureSlots> m_TextureSamplerSlots{}; ///< Zero-init IS DefaultSlot.
        std::array<u32, kMaxTextureSlots> m_ImageHeapSlots{};
        VulkanFramebuffer* m_CurrentFramebuffer = nullptr;

        bool m_TextureSlotsInitialised = false;
        void EnsureTextureSlotsInitialised();
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN

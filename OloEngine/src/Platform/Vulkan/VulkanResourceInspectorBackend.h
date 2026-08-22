#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"

// NOTE: no ImGui header here, and no Vulkan header either — the panels live in
// Renderer/Debug/ (ADR 0011 §1.6) and the volk/VMA includes stay in the .cpp,
// so this header is includable from the neutral factory TU.

namespace OloEngine
{
    // @brief Vulkan implementation of GPUResourceInspector's data plane (#810).
    //
    // WHERE ITS ANSWERS COME FROM, and the mapping decision behind them.
    //
    // The GL arm is PUSHED: every GL resource constructor calls an
    // OLO_GPU_REGISTER_* macro, so the shell's map is authoritative. No Vulkan
    // TU calls those macros, and adding ~15 more registration call sites would
    // duplicate bookkeeping the engine already does — every backend resource
    // registers with RHI::ResourceRegistry at creation. So this arm is PULLED:
    // DiscoverResources() takes a ResourceRegistry::Snapshot() (the one place
    // that already holds BOTH currencies for both backends), keeps the
    // Vulkan-owned rows, and enriches each one from the backend-internal side
    // tables:
    //
    //   * RHI::ResourceKind::Texture     -> VulkanImageInfoRegistry (extent,
    //                                       VkFormat, mips, layers, view type)
    //   * RHI::ResourceKind::Buffer      -> VulkanRootObjectRegistry (vertex /
    //                                       index / uniform / storage objects,
    //                                       for their sizes) then
    //                                       VulkanRawBufferRegistry (the
    //                                       object-less raw family)
    //   * RHI::ResourceKind::Framebuffer -> VulkanRootObjectRegistry (its
    //                                       specification's extent). Its
    //                                       registry native is 0 by design —
    //                                       there is no VkFramebuffer under
    //                                       dynamic rendering — which is
    //                                       exactly why the shell keys its map
    //                                       on the identity, not the native.
    //   * VertexArray / ShaderProgram    -> VulkanRootObjectRegistry (name only)
    //
    // WHY NOT KEY ON THE NATIVE HANDLE. A VkImage's handle VALUE is recyclable
    // by the driver, and several live Vulkan resources legitimately have no
    // native object at all (framebuffers, arena-backed UBOs, VAOs). The
    // identity is unique and stable by construction; the native is what a
    // RenderDoc / RGP capture shows. Amendment (77) says surface both, and
    // neither is derivable from the other — so both travel on every row.
    //
    // WHAT THIS ARM DELIBERATELY DOES NOT DO. Texture PREVIEWS and the async
    // download engine stay GL-only: they are a PBO + fence pipeline feeding the
    // GL ImGui renderer backend, neither of which exists here. Those entry
    // points return "unsupported" rather than a plausible wrong answer, and the
    // panel says so. Pixel inspection under Vulkan goes through
    // olo_render_capture_target / olo_render_probe_pixel / olo_render_target_stats,
    // which read via the facade readback spine (RenderCommand::ReadTextureSubImage).
    //
    // THREADING. VulkanImageInfoRegistry and VulkanRootObjectRegistry are
    // documented render-thread-only, so every entry point here must be called
    // from the render thread — GPUResourceInspector::RefreshDiscoveredResources
    // carries that contract to its callers, and the MCP tool marshals.
    class VulkanResourceInspectorBackend final : public IResourceInspectorBackend
    {
      public:
        ~VulkanResourceInspectorBackend() override = default;

        // ---- Registry-driven discovery -------------------------------------
        [[nodiscard]] bool DiscoversResources() const override
        {
            return true;
        }
        void DiscoverResources(std::vector<DiscoveredResource>& out) override;
        [[nodiscard]] bool QueryMemoryHeaps(std::vector<MemoryHeap>& out) override;

        // ---- Introspection -------------------------------------------------
        void QueryTexture(u64 nativeTextureId, bool isCubemap, TextureQuery& outInfo) override;
        void QueryBuffer(u64 nativeBufferId, u32 nativeTarget, BufferQuery& outInfo) override;
        void QueryFramebuffer(u64 nativeFramebufferId, FramebufferQuery& outInfo) override;
        [[nodiscard]] BufferKind ClassifyBufferTarget(u32 nativeTarget) const override;
        [[nodiscard]] u64 GetBoundTexture2D() const override;
        void GetTextureLevelSize(u64 nativeTextureId, u32 mipLevel, u32& outWidth, u32& outHeight) override;

        // ---- Native-enum vocabulary ----------------------------------------
        [[nodiscard]] i32 ChannelCountForPixelFormat(u32 nativePixelFormat) const override;
        [[nodiscard]] PixelPrecision ClassifyPixelDataType(u32 nativeDataType) const override;
        [[nodiscard]] bool IsFloatPixelDataType(u32 nativeDataType) const override;
        [[nodiscard]] std::string FormatTextureFormatName(u32 nativeInternalFormat) const override;
        [[nodiscard]] std::string FormatBufferUsageName(u32 nativeUsage) const override;
        [[nodiscard]] const char* GetBufferTargetName(u32 nativeTarget) const override;
        [[nodiscard]] const char* FramebufferStatusName(u32 nativeStatus, FramebufferStatusClass& outClass) const override;

        // ---- Synchronous readback ------------------------------------------
        // Unsupported here (see the class comment): these are keyed on a native
        // id and a GL pixel-format enum, while the Vulkan readback spine is
        // keyed on an RHI handle. They fail with an explanatory error rather
        // than reading nothing and reporting success.
        bool ReadTextureLevel(u64 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                              u32 width, u32 height, u32 nativePixelFormat, bool readAsFloat,
                              void* dest, sizet destBytes, std::string& outError) override;
        bool ReadBufferRange(u64 nativeBufferId, u32 nativeTarget, u32 offset, u32 size, void* dest) override;

        // ---- Texture capture -----------------------------------------------
        bool QueryCaptureSource(u64 nativeTextureId, u32 mipLevel, CaptureSource& outSource) override;
        bool ReadCaptureRegion(u64 nativeTextureId, u32 mipLevel, u32 faceOrLayer,
                               const CaptureSource& source, u32 regionX, u32 regionY,
                               u32 regionWidth, u32 regionHeight,
                               void* dest, sizet destBytes, std::string& outError) override;
        [[nodiscard]] bool CaptureRowsAreBottomUp() const override;

        // ---- Async download engine -----------------------------------------
        bool BeginTextureDownload(u64 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                  u32 width, u32 height, sizet dataSize, DownloadTicket& outTicket) override;
        [[nodiscard]] DownloadStatus PollDownload(const DownloadTicket& ticket) override;
        [[nodiscard]] const void* MapDownloadData(const DownloadTicket& ticket, sizet dataSize) override;
        void UnmapDownloadData(const DownloadTicket& ticket) override;
        void ReleaseDownload(const DownloadTicket& ticket) override;

        // ---- ImGui binding -------------------------------------------------
        [[nodiscard]] u64 GetImGuiTextureID(u64 nativeTextureId) const override;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN

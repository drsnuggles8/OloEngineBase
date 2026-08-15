#pragma once

#include "OloEngine/Core/Base.h"

#include <memory>
#include <string>
#include <vector>

namespace OloEngine
{
    // @brief Backend data plane for GPUResourceInspector (#691 Phase 9, ADR 0011 §1.6).
    //
    // GPUResourceInspector keeps the registration bookkeeping, the download-
    // request lifecycle and the ImGui presentation; everything that must touch
    // the graphics API to answer a question — introspection queries, pixel and
    // buffer readbacks, the PBO/fence async download engine — goes through this
    // interface. It deliberately speaks the two currencies of ADR 0011
    // amendment (77): u32 NATIVE ids / native enum values (backend implied —
    // useful raw, e.g. for RenderDoc correlation) plus opaque void* fences,
    // never GL types, so Renderer/Debug/ compiles without <glad/gl.h>.
    //
    // Today the only implementation is OpenGLResourceInspectorBackend
    // (Platform/OpenGL/). Under Vulkan the factory intentionally returns null
    // — a deliberate, documented stub: the OLO_GPU_REGISTER_* macros are
    // called only from Platform/OpenGL TUs, so a Vulkan session has nothing
    // registered and the inspector stays un-initialized (exactly like the
    // glOnlyTooling gating in Application.cpp); Vulkan resource visibility
    // comes from the VMA/root-object registries instead.
    class IResourceInspectorBackend
    {
      public:
        // Coarse buffer classification derived from the native buffer-target
        // enum — the neutral shell cannot name GL_ARRAY_BUFFER et al.
        enum class BufferKind : u8
        {
            Vertex = 0,
            Index,
            Uniform
        };

        // Readback precision for a native pixel data-type enum. `Unsupported`
        // covers packed depth-stencil and true integer texture formats.
        enum class PixelPrecision : u8
        {
            UnsignedByte = 0,
            Float,
            Unsupported
        };

        enum class DownloadStatus : u8
        {
            Pending = 0,
            Complete,
            Failed
        };

        // Severity bucket for a native framebuffer-status enum, for UI coloring.
        enum class FramebufferStatusClass : u8
        {
            Complete = 0,
            Incomplete,
            Unsupported,
            Unknown
        };

        struct TextureQuery
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 MipLevels = 1;
            bool HasMips = false;
            // Native (backend) enum values, for RenderDoc correlation.
            u32 InternalFormat = 0;
            u32 PixelFormat = 0;
            u32 DataType = 0;
            sizet MemoryUsage = 0;
        };

        struct BufferQuery
        {
            u32 Size = 0;
            u32 Usage = 0; // native (backend) usage enum value
            sizet MemoryUsage = 0;
        };

        struct FramebufferQuery
        {
            u32 Width = 0;
            u32 Height = 0;
            u32 Status = 0; // native (backend) framebuffer-status enum value
            u32 ColorAttachmentCount = 0;
            std::vector<u32> ColorAttachmentFormats; // native enum values
            bool HasDepthAttachment = false;
            u32 DepthAttachmentFormat = 0; // native enum value
            bool HasStencilAttachment = false;
            u32 StencilAttachmentFormat = 0; // native enum value
            sizet MemoryUsage = 0;
        };

        // Everything CaptureTexturePng needs to know about a capture source
        // before reading pixels. On failure `Error` is non-empty and the query
        // returns false.
        struct CaptureSource
        {
            u32 FullWidth = 0;
            u32 FullHeight = 0;
            i32 Channels = 0;
            bool IsFloat = false;
            bool IsDepth = false;
            // Whether faceOrLayer selects a cubemap face / array layer / 3D
            // slice for this texture — backend-internal detail the shell hands
            // back to ReadCaptureRegion unchanged.
            bool Layered = false;
            u32 PixelFormat = 0; // native readback format enum value
            std::string FormatName;
            std::string Error;
        };

        // Opaque handles for one in-flight async texture download. The shell
        // stores these in its TextureDownloadRequest and hands them back for
        // polling / mapping / release; only the backend interprets them.
        struct DownloadTicket
        {
            u32 NativeBuffer = 0;  // staging pixel buffer (PBO on GL)
            void* Fence = nullptr; // opaque native fence (GLsync on GL)
        };

        virtual ~IResourceInspectorBackend() = default;

        // ---- Introspection -------------------------------------------------
        virtual void QueryTexture(u32 nativeTextureId, bool isCubemap, TextureQuery& outInfo) = 0;
        virtual void QueryBuffer(u32 nativeBufferId, u32 nativeTarget, BufferQuery& outInfo) = 0;
        virtual void QueryFramebuffer(u32 nativeFramebufferId, FramebufferQuery& outInfo) = 0;
        [[nodiscard]] virtual BufferKind ClassifyBufferTarget(u32 nativeTarget) const = 0;
        // The texture bound to the 2D target of the active texture unit (0 = none).
        [[nodiscard]] virtual u32 GetBoundTexture2D() const = 0;
        // Mip-level dimensions (0 when the level has no storage / id invalid).
        virtual void GetTextureLevelSize(u32 nativeTextureId, u32 mipLevel, u32& outWidth, u32& outHeight) = 0;

        // ---- Native-enum vocabulary (decoding for display / decisions) -----
        [[nodiscard]] virtual i32 ChannelCountForPixelFormat(u32 nativePixelFormat) const = 0;
        [[nodiscard]] virtual PixelPrecision ClassifyPixelDataType(u32 nativeDataType) const = 0;
        [[nodiscard]] virtual bool IsFloatPixelDataType(u32 nativeDataType) const = 0;
        [[nodiscard]] virtual std::string FormatTextureFormatName(u32 nativeInternalFormat) const = 0;
        [[nodiscard]] virtual std::string FormatBufferUsageName(u32 nativeUsage) const = 0;
        [[nodiscard]] virtual const char* GetBufferTargetName(u32 nativeTarget) const = 0;
        [[nodiscard]] virtual const char* FramebufferStatusName(u32 nativeStatus, FramebufferStatusClass& outClass) const = 0;

        // ---- Synchronous readback ------------------------------------------
        // Read one mip/face of a texture into `dest` (tightly packed rows, raw
        // GPU row order). `readAsFloat` selects f32 vs u8 per-channel output.
        // On failure fills `outError` and returns false.
        virtual bool ReadTextureLevel(u32 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                      u32 width, u32 height, u32 nativePixelFormat, bool readAsFloat,
                                      void* dest, sizet destBytes, std::string& outError) = 0;
        // Copy `size` bytes at `offset` out of a buffer object. Returns false
        // when the buffer cannot be mapped.
        virtual bool ReadBufferRange(u32 nativeBufferId, u32 nativeTarget, u32 offset, u32 size, void* dest) = 0;

        // ---- Texture capture (CaptureTexturePng's data-gathering) ----------
        virtual bool QueryCaptureSource(u32 nativeTextureId, u32 mipLevel, CaptureSource& outSource) = 0;
        // Read a sub-rect (top-left-origin coordinates; the backend converts to
        // its own row origin) of one mip/face into `dest`, tightly packed, in
        // the backend's native row order — flip afterwards iff
        // CaptureRowsAreBottomUp().
        virtual bool ReadCaptureRegion(u32 nativeTextureId, u32 mipLevel, u32 faceOrLayer,
                                       const CaptureSource& source, u32 regionX, u32 regionY,
                                       u32 regionWidth, u32 regionHeight,
                                       void* dest, sizet destBytes, std::string& outError) = 0;
        [[nodiscard]] virtual bool CaptureRowsAreBottomUp() const = 0;

        // ---- Async download engine -----------------------------------------
        // Kick an async RGBA8 readback of one mip/face into a staging buffer
        // with a completion fence. Returns false (nothing to release) on failure.
        virtual bool BeginTextureDownload(u32 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                          u32 width, u32 height, sizet dataSize, DownloadTicket& outTicket) = 0;
        [[nodiscard]] virtual DownloadStatus PollDownload(const DownloadTicket& ticket) = 0;
        // Map the completed download for reading; returns null on failure.
        // Every successful map MUST be paired with UnmapDownloadData.
        [[nodiscard]] virtual const void* MapDownloadData(const DownloadTicket& ticket, sizet dataSize) = 0;
        virtual void UnmapDownloadData(const DownloadTicket& ticket) = 0;
        virtual void ReleaseDownload(const DownloadTicket& ticket) = 0;

        // ---- ImGui binding -------------------------------------------------
        // A value usable as ImTextureID by the active ImGui renderer backend
        // (the raw texture name on GL). 0 = no binding — callers must skip the
        // ImGui::Image draw, same contract as ImGuiLayer::GetTextureID.
        [[nodiscard]] virtual u64 GetImGuiTextureID(u32 nativeTextureId) const = 0;
    };

    // Factory — the one place the backend switch lives. Returns null when the
    // active RendererAPI has no inspector backend (the deliberate Vulkan stub
    // — see the class comment above); defined in ResourceInspectorBackend.cpp,
    // the TU sanctioned to include Platform headers (Framebuffer.cpp's
    // factory-include pattern).
    [[nodiscard]] std::unique_ptr<IResourceInspectorBackend> CreateResourceInspectorBackend();
} // namespace OloEngine

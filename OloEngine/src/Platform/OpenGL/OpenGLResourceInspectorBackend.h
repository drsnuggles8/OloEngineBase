#pragma once

#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"

// NOTE: no ImGui header here, ever — Platform/OpenGL must stay
// presentation-free (ADR 0011 §1.6); the ImGui panels live in Renderer/Debug/.
// The GL calls themselves live in the .cpp, so this header needs no glad
// include either.

namespace OloEngine
{
    // @brief OpenGL implementation of GPUResourceInspector's data plane
    //        (#691, ADR 0011 §1.6).
    //
    // Holds everything that used to be the inspector's direct glad usage: the
    // DSA introspection queries, the synchronous glGetTextureSubImage
    // readbacks (with their pack-state and PBO-unbind hygiene), the PBO +
    // fence async download engine, and the GL-enum decoding tables. The class
    // is stateless — all per-download state travels in the shell's
    // TextureDownloadRequest via DownloadTicket — so one instance serves the
    // whole process.
    class OpenGLResourceInspectorBackend final : public IResourceInspectorBackend
    {
      public:
        ~OpenGLResourceInspectorBackend() override = default;

        // ---- Introspection -------------------------------------------------
        void QueryTexture(u32 nativeTextureId, bool isCubemap, TextureQuery& outInfo) override;
        void QueryBuffer(u32 nativeBufferId, u32 nativeTarget, BufferQuery& outInfo) override;
        void QueryFramebuffer(u32 nativeFramebufferId, FramebufferQuery& outInfo) override;
        [[nodiscard]] BufferKind ClassifyBufferTarget(u32 nativeTarget) const override;
        [[nodiscard]] u32 GetBoundTexture2D() const override;
        void GetTextureLevelSize(u32 nativeTextureId, u32 mipLevel, u32& outWidth, u32& outHeight) override;

        // ---- Native-enum vocabulary ----------------------------------------
        [[nodiscard]] i32 ChannelCountForPixelFormat(u32 nativePixelFormat) const override;
        [[nodiscard]] PixelPrecision ClassifyPixelDataType(u32 nativeDataType) const override;
        [[nodiscard]] bool IsFloatPixelDataType(u32 nativeDataType) const override;
        [[nodiscard]] std::string FormatTextureFormatName(u32 nativeInternalFormat) const override;
        [[nodiscard]] std::string FormatBufferUsageName(u32 nativeUsage) const override;
        [[nodiscard]] const char* GetBufferTargetName(u32 nativeTarget) const override;
        [[nodiscard]] const char* FramebufferStatusName(u32 nativeStatus, FramebufferStatusClass& outClass) const override;

        // ---- Synchronous readback ------------------------------------------
        bool ReadTextureLevel(u32 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                              u32 width, u32 height, u32 nativePixelFormat, bool readAsFloat,
                              void* dest, sizet destBytes, std::string& outError) override;
        bool ReadBufferRange(u32 nativeBufferId, u32 nativeTarget, u32 offset, u32 size, void* dest) override;

        // ---- Texture capture -----------------------------------------------
        bool QueryCaptureSource(u32 nativeTextureId, u32 mipLevel, CaptureSource& outSource) override;
        bool ReadCaptureRegion(u32 nativeTextureId, u32 mipLevel, u32 faceOrLayer,
                               const CaptureSource& source, u32 regionX, u32 regionY,
                               u32 regionWidth, u32 regionHeight,
                               void* dest, sizet destBytes, std::string& outError) override;
        [[nodiscard]] bool CaptureRowsAreBottomUp() const override;

        // ---- Async download engine -----------------------------------------
        bool BeginTextureDownload(u32 nativeTextureId, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                  u32 width, u32 height, sizet dataSize, DownloadTicket& outTicket) override;
        [[nodiscard]] DownloadStatus PollDownload(const DownloadTicket& ticket) override;
        [[nodiscard]] const void* MapDownloadData(const DownloadTicket& ticket, sizet dataSize) override;
        void UnmapDownloadData(const DownloadTicket& ticket) override;
        void ReleaseDownload(const DownloadTicket& ticket) override;

        // ---- ImGui binding -------------------------------------------------
        [[nodiscard]] u64 GetImGuiTextureID(u32 nativeTextureId) const override;

      private:
        // Buffer binding utility
        [[nodiscard]] static u32 GetBufferBindingQuery(u32 target);

        // Texture memory calculation utilities
        [[nodiscard]] sizet CalculateAccurateTextureMemoryUsage(u32 textureId, u32 target, u32 internalFormat,
                                                                u32 width, u32 height, u32 mipLevels) const;
        [[nodiscard]] sizet CalculateCompressedTextureMemory(u32 textureId, u32 target, u32 internalFormat,
                                                             u32 /*width*/, u32 /*height*/, u32 mipLevels) const;
        [[nodiscard]] sizet CalculateUncompressedTextureMemory(u32 width, u32 height, u32 bytesPerPixel, u32 mipLevels) const;
        [[nodiscard]] u32 GetUncompressedBytesPerPixel(u32 internalFormat) const;
        [[nodiscard]] u32 GetCompressedBlockSize(u32 internalFormat) const;
    };
} // namespace OloEngine

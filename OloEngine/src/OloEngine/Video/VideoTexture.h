#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine
{
    /// Streams decoded RGBA8 video frames into a GPU texture for sampling by the
    /// fullscreen overlay pass or a world-space material.
    ///
    /// GL-backed: Initialize / UpdateFrame / Bind must run on the thread that owns the
    /// renderer's GL context (the main thread, during Scene::OnUpdateRuntime / rendering).
    /// Every method no-ops until Initialize() has created the texture, so a VideoPlayer
    /// can decode headlessly (e.g. in unit tests with no GL context) without touching GL.
    ///
    /// The backing texture is created with TextureSpecification::Streaming, so the GL
    /// backend uploads each frame through a double-buffered Pixel Buffer Object ring
    /// (orphan + UNSYNCHRONIZED map) — the CPU copy and GPU DMA overlap instead of stalling.
    class VideoTexture
    {
      public:
        VideoTexture() = default;
        ~VideoTexture() = default;

        VideoTexture(const VideoTexture&) = delete;
        VideoTexture& operator=(const VideoTexture&) = delete;

        /// (Re)create the backing texture for frames of the given size. No-op if the
        /// size is unchanged or zero.
        void Initialize(u32 width, u32 height);
        void Destroy();

        /// Upload a tightly packed RGBA8 frame (width*height*4 bytes). Recreates the
        /// texture if the dimensions changed. No-op on null data or zero size.
        void UpdateFrame(const u8* rgbaData, u32 width, u32 height);

        void Bind(u32 slot = 0) const;

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Texture != nullptr;
        }
        [[nodiscard]] u32 GetRendererID() const;
        [[nodiscard]] u32 GetWidth() const
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const
        {
            return m_Height;
        }
        [[nodiscard]] const Ref<Texture2D>& GetTexture() const
        {
            return m_Texture;
        }

      private:
        Ref<Texture2D> m_Texture = nullptr;
        u32 m_Width = 0;
        u32 m_Height = 0;
    };
} // namespace OloEngine

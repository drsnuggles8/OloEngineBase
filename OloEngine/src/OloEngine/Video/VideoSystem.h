#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Video/VideoPlayer.h"

#include <functional>
#include <string>

namespace OloEngine
{
    class Scene;

    /// Per-frame driver for video-playback components, plus a process-global fullscreen
    /// overlay player for cutscenes / splash screens.
    ///
    /// OnUpdate must run on the main (GL-owning) thread — it both advances playback clocks
    /// and uploads frames to GPU textures. It is called from Scene::OnUpdateRuntime.
    class VideoSystem
    {
      public:
        /// Tick every video component in @p scene and the global fullscreen player.
        static void OnUpdate(Scene* scene, f32 dt);

        /// Release the global fullscreen player. Call on engine/scene shutdown.
        static void Shutdown();

        // --- Global fullscreen overlay (rendered on top of the scene). ---
        static void PlayFullscreen(const std::string& filePath, bool loop = false, std::function<void()> onFinished = {});
        /// Show a single static RGBA8 image fullscreen (splash / studio logo), composited by
        /// the same overlay path as video. Width*height*4 bytes; the buffer is copied.
        static void ShowFullscreenImage(const u8* rgba, u32 width, u32 height);
        static void StopFullscreen();
        [[nodiscard]] static bool IsFullscreenPlaying();
        [[nodiscard]] static Ref<VideoPlayer> GetFullscreenPlayer();
        /// Stop a skippable fullscreen video early (fires its OnFinished). No-op otherwise.
        static void SkipFullscreen();
        static void SetFullscreenSkippable(bool skippable);
        [[nodiscard]] static bool IsFullscreenSkippable();

        // --- World-space ---
        /// Create a standalone player for binding to a mesh material. Returns null on failure.
        [[nodiscard]] static Ref<VideoPlayer> CreateWorldPlayer(const std::string& filePath);
    };
} // namespace OloEngine

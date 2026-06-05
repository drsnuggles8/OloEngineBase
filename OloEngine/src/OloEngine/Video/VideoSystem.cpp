#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoSystem.h"

#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <utility>

namespace OloEngine
{
    namespace
    {
        Ref<VideoPlayer> s_FullscreenPlayer = nullptr;
        bool s_FullscreenSkippable = true;

        // Resolve an asset-relative video path to a filesystem path the decoder can open.
        // Absolute paths pass through unchanged (std::filesystem operator/ semantics).
        std::string ResolveVideoPath(const std::string& path)
        {
            if (path.empty())
                return path;
            if (!Project::GetActive())
                return path;
            return Project::GetAssetFileSystemPath(path).string();
        }

        // A common "skip the cutscene" gesture: Esc / Space / Enter just pressed this frame.
        bool SkipInputPressed()
        {
            return Input::IsKeyJustPressed(Key::Escape) || Input::IsKeyJustPressed(Key::Space) || Input::IsKeyJustPressed(Key::Enter);
        }

        // Lazily create + start a player for a component, applying its authored settings.
        Ref<VideoPlayer> CreateAndStart(const std::string& assetPath, bool loop, f32 volume)
        {
            auto player = Ref<VideoPlayer>::Create();
            if (!player->Load(ResolveVideoPath(assetPath)))
            {
                OLO_CORE_WARN("VideoSystem - failed to load video '{}'", assetPath);
                return nullptr;
            }
            player->SetLooping(loop);
            player->SetVolume(volume);
            player->Play();
            return player;
        }
    } // namespace

    void VideoSystem::PlayFullscreen(const std::string& filePath, bool loop, std::function<void()> onFinished)
    {
        StopFullscreen();

        auto player = Ref<VideoPlayer>::Create();
        if (!player->Load(ResolveVideoPath(filePath)))
        {
            OLO_CORE_WARN("VideoSystem::PlayFullscreen - failed to load '{}'", filePath);
            return;
        }

        player->SetLooping(loop);
        player->OnFinished = [cb = std::move(onFinished)]()
        {
            if (cb)
                cb();
        };
        player->Play();

        s_FullscreenPlayer = player;
        s_FullscreenSkippable = true;
    }

    void VideoSystem::ShowFullscreenImage(const u8* rgba, u32 width, u32 height)
    {
        StopFullscreen();
        if (!rgba || width == 0 || height == 0)
            return;

        auto player = Ref<VideoPlayer>::Create();
        player->PresentImage(rgba, width, height);
        s_FullscreenPlayer = player;
        s_FullscreenSkippable = true;
    }

    void VideoSystem::StopFullscreen()
    {
        if (s_FullscreenPlayer)
        {
            s_FullscreenPlayer->Unload();
            s_FullscreenPlayer = nullptr;
        }
    }

    bool VideoSystem::IsFullscreenPlaying()
    {
        return s_FullscreenPlayer && s_FullscreenPlayer->IsPlaying();
    }

    Ref<VideoPlayer> VideoSystem::GetFullscreenPlayer()
    {
        return s_FullscreenPlayer;
    }

    void VideoSystem::SkipFullscreen()
    {
        if (!s_FullscreenPlayer || !s_FullscreenSkippable)
            return;

        // Keep the current player alive across the callback, which may itself start a new
        // fullscreen video (PlayFullscreen). Only tear down if it's still the same player —
        // otherwise we'd immediately kill the one OnFinished just started.
        Ref<VideoPlayer> current = s_FullscreenPlayer;
        if (current->OnFinished)
            current->OnFinished();
        if (s_FullscreenPlayer.Raw() == current.Raw())
            StopFullscreen();
    }

    void VideoSystem::SetFullscreenSkippable(bool skippable)
    {
        s_FullscreenSkippable = skippable;
    }

    bool VideoSystem::IsFullscreenSkippable()
    {
        return s_FullscreenSkippable;
    }

    Ref<VideoPlayer> VideoSystem::CreateWorldPlayer(const std::string& filePath)
    {
        auto player = Ref<VideoPlayer>::Create();
        if (!player->Load(ResolveVideoPath(filePath)))
        {
            OLO_CORE_WARN("VideoSystem::CreateWorldPlayer - failed to load '{}'", filePath);
            return nullptr;
        }
        return player;
    }

    void VideoSystem::Shutdown()
    {
        StopFullscreen();
    }

    void VideoSystem::OnUpdate(Scene* scene, f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        // --- Global fullscreen overlay (cutscenes / splash screens). ---
        if (s_FullscreenPlayer)
        {
            if (s_FullscreenSkippable && s_FullscreenPlayer->IsPlaying() && SkipInputPressed())
            {
                SkipFullscreen();
            }
            else
            {
                s_FullscreenPlayer->Update(dt);
                s_FullscreenPlayer->UpdateTexture();
                if (s_FullscreenPlayer->IsFinished())
                    s_FullscreenPlayer = nullptr;
            }
        }

        if (!scene)
            return;

        // --- Per-entity fullscreen overlays (VideoOverlayComponent). ---
        for (auto view = scene->GetAllEntitiesWith<VideoOverlayComponent>(); auto e : view)
        {
            auto& comp = view.get<VideoOverlayComponent>(e);

            if (!comp.Player && comp.PlayOnStart && !comp.VideoPath.empty())
                comp.Player = CreateAndStart(comp.VideoPath, comp.Looping, comp.Volume);

            if (!comp.Player)
                continue;

            if (comp.SkipOnInput && comp.Player->IsPlaying() && SkipInputPressed())
                comp.Player->Stop();

            comp.Player->SetLooping(comp.Looping);
            comp.Player->SetVolume(comp.Volume);
            comp.Player->Update(dt);
            comp.Player->UpdateTexture();
        }

        // --- World-space video surfaces (VideoSurfaceComponent). ---
        for (auto view = scene->GetAllEntitiesWith<VideoSurfaceComponent>(); auto e : view)
        {
            auto& comp = view.get<VideoSurfaceComponent>(e);

            if (!comp.Player && comp.AutoPlay && !comp.VideoPath.empty())
                comp.Player = CreateAndStart(comp.VideoPath, comp.Looping, comp.Volume);

            if (!comp.Player)
                continue;

            comp.Player->SetLooping(comp.Looping);
            comp.Player->SetVolume(comp.Volume);
            comp.Player->Update(dt);
            comp.Player->UpdateTexture();

            // Drive the entity material's albedo from the decoded frame.
            Entity entity{ e, scene };
            if (entity.HasComponent<MaterialComponent>() && comp.Player->GetTexture().IsInitialized())
            {
                auto& matComp = entity.GetComponent<MaterialComponent>();
                matComp.m_Material.SetAlbedoMap(comp.Player->GetTexture().GetTexture());
            }
        }
    }
} // namespace OloEngine

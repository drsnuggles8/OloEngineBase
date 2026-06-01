#include "OloEnginePCH.h"
#include "OloEngine/Cinematic/CinematicSystem.h"

#include "OloEngine/Cinematic/CinematicComponent.h"
#include "OloEngine/Cinematic/CinematicPlayer.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"

#include <optional>

namespace OloEngine
{
    Ref<CinematicSequence> CinematicSystem::ResolveSequence(CinematicComponent& component)
    {
        if (component.RuntimeSequence)
        {
            return component.RuntimeSequence;
        }
        if (component.Sequence != 0)
        {
            component.RuntimeSequence = AssetManager::GetAsset<CinematicSequence>(component.Sequence);
        }
        return component.RuntimeSequence;
    }

    void CinematicSystem::ApplyAtTime(Scene& scene, const CinematicSequence& sequence, f32 time)
    {
        OLO_PROFILE_FUNCTION();

        for (const auto& track : sequence.TransformTracks)
        {
            auto target = scene.TryGetEntityWithUUID(track.Target);
            if (!target || !target->HasComponent<TransformComponent>())
            {
                continue;
            }
            auto& transform = target->GetComponent<TransformComponent>();
            if (track.Translation.HasKeys())
            {
                transform.Translation = track.Translation.Evaluate(time, transform.Translation);
            }
            if (track.Rotation.HasKeys())
            {
                transform.SetRotation(track.Rotation.Evaluate(time, transform.GetRotation()));
            }
            if (track.Scale.HasKeys())
            {
                transform.Scale = track.Scale.Evaluate(time, transform.Scale);
            }
        }

        for (const auto& track : sequence.CameraTracks)
        {
            auto target = scene.TryGetEntityWithUUID(track.Target);
            if (!target)
            {
                continue;
            }
            if (target->HasComponent<TransformComponent>())
            {
                auto& transform = target->GetComponent<TransformComponent>();
                if (track.Position.HasKeys())
                {
                    transform.Translation = track.Position.Evaluate(time, transform.Translation);
                }
                if (track.Rotation.HasKeys())
                {
                    transform.SetRotation(track.Rotation.Evaluate(time, transform.GetRotation()));
                }
            }
            if (track.VerticalFovRadians.HasKeys() && target->HasComponent<CameraComponent>())
            {
                auto& camera = target->GetComponent<CameraComponent>();
                const f32 fov = track.VerticalFovRadians.Evaluate(time, camera.Camera.GetPerspectiveVerticalFOV());
                camera.Camera.SetPerspectiveVerticalFOV(fov);
            }
        }

        for (const auto& track : sequence.VisibilityTracks)
        {
            if (track.Keys.empty())
            {
                continue;
            }
            auto target = scene.TryGetEntityWithUUID(track.Target);
            if (!target || !target->HasComponent<ModelComponent>())
            {
                continue;
            }
            auto& model = target->GetComponent<ModelComponent>();
            model.m_Visible = track.EvaluateAt(time, model.m_Visible);
        }
    }

    void CinematicSystem::Advance(Scene& scene, CinematicComponent& component, CinematicSequence& sequence, f32 dt)
    {
        const CinematicPlayer::TickResult tick =
            CinematicPlayer::Tick(sequence, component.Time, component.PreviousTime, dt, component.PlaybackSpeed, component.Loop);

        component.PreviousTime = tick.NewTime;
        component.Time = tick.NewTime;
        component.EventsFiredThisFrame = tick.FiredEvents;

        // Sample continuous tracks at the new playhead.
        ApplyAtTime(scene, sequence, component.Time);

        if (tick.JustFinished)
        {
            component.Playing = false;
            component.Finished = true;
        }
    }

    void CinematicSystem::Update(Scene& scene, Timestep ts)
    {
        OLO_PROFILE_FUNCTION();

        const f32 dt = ts.GetSeconds();

        for (auto entityHandle : scene.GetAllEntitiesWith<CinematicComponent>())
        {
            Entity entity{ entityHandle, &scene };
            auto& component = entity.GetComponent<CinematicComponent>();

            component.EventsFiredThisFrame.clear();

            if (!component.Playing)
            {
                continue;
            }

            Ref<CinematicSequence> sequence = ResolveSequence(component);
            if (!sequence)
            {
                // Handle set but asset missing/unloaded — stay idle, don't spin.
                component.Playing = false;
                continue;
            }

            Advance(scene, component, *sequence, dt);
        }
    }
} // namespace OloEngine

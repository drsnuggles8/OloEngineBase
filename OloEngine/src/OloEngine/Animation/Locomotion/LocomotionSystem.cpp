#include "OloEnginePCH.h"
#include "OloEngine/Animation/Locomotion/LocomotionSystem.h"

#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/LocomotionComponent.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine::Animation
{
    namespace
    {
        // Define-if-missing then set: a graph only references the parameter
        // names it cares about; defining the rest is harmless and avoids the
        // per-tick "parameter not found" warning spam.
        void WriteFloat(AnimationParameterSet& params, const std::string& name, f32 value)
        {
            if (name.empty())
            {
                return;
            }
            if (!params.HasParameter(name))
            {
                params.DefineFloat(name, value);
                return;
            }
            params.SetFloat(name, value);
        }

        void WriteInt(AnimationParameterSet& params, const std::string& name, i32 value)
        {
            if (name.empty())
            {
                return;
            }
            if (!params.HasParameter(name))
            {
                params.DefineInt(name, value);
                return;
            }
            params.SetInt(name, value);
        }

        // Gait selection with hysteresis: enter thresholds move up the ladder,
        // exit thresholds (lower) move down — a speed hovering at a boundary
        // can't flicker.
        i32 SelectGait(i32 currentGait, f32 speed, const LocomotionComponent& loco)
        {
            i32 gait = currentGait;
            // Move up as far as the speed allows.
            if (gait < 1 && speed >= loco.WalkEnterSpeed)
            {
                gait = 1;
            }
            if (gait == 1 && speed >= loco.RunEnterSpeed)
            {
                gait = 2;
            }
            // Move down when the speed falls below the (lower) exit thresholds.
            if (gait == 2 && speed < loco.RunExitSpeed)
            {
                gait = 1;
            }
            if (gait >= 1 && speed < loco.WalkExitSpeed)
            {
                gait = 0;
            }
            return gait;
        }

        // Extract yaw (heading) from a rotation: the angle of the rotated +Z in
        // the XZ plane.
        f32 YawOf(const glm::quat& rotation)
        {
            const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            return std::atan2(forward.x, forward.z);
        }
    } // namespace

    void LocomotionSystem::OnUpdate(Scene* scene, f32 deltaTime)
    {
        OLO_PROFILE_FUNCTION();

        if (deltaTime <= 0.0f)
        {
            return;
        }

        auto view = scene->GetAllEntitiesWith<LocomotionComponent, AnimationGraphComponent, TransformComponent>();
        for (auto e : view)
        {
            auto& loco = view.template get<LocomotionComponent>(e);
            if (!loco.Enabled)
            {
                continue;
            }

            Entity entity{ e, scene };
            if (!entity.HasComponent<LocomotionStateComponent>())
            {
                entity.AddComponent<LocomotionStateComponent>();
            }
            auto& state = entity.GetComponent<LocomotionStateComponent>();
            auto& transform = view.template get<TransformComponent>(e);
            auto& graphComp = view.template get<AnimationGraphComponent>(e);

            // ── Velocity: desired (root-motion steering) or measured ─────────
            glm::vec3 velocity{ 0.0f };
            if (loco.UseDesiredVelocity)
            {
                velocity = loco.DesiredVelocity;
            }
            else
            {
                bool haveVelocity = false;
                if (auto* joltScene = scene->GetPhysicsScene(); joltScene && entity.HasComponent<CharacterController3DComponent>())
                {
                    if (auto controller = joltScene->GetCharacterController(entity))
                    {
                        velocity = controller->GetLinearVelocity();
                        haveVelocity = true;
                    }
                }
                if (!haveVelocity && state.HasPrev)
                {
                    velocity = (transform.Translation - state.PrevPosition) / deltaTime;
                }
            }
            if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
            {
                velocity = glm::vec3(0.0f);
            }

            const glm::quat rotation = transform.GetRotation();
            const f32 rawYaw = YawOf(rotation);
            // A non-finite transform rotation (NaN injected by a script/physics
            // glitch) would poison SmoothedTurnRate and PrevYaw permanently — keep
            // the last finite yaw and skip this frame's turn-rate update instead.
            const bool yawValid = std::isfinite(rawYaw);
            const f32 yaw = yawValid ? rawYaw : state.PrevYaw;

            // ── Smoothing ─────────────────────────────────────────────────────
            const f32 alpha = 1.0f - std::exp(-loco.SpeedSmoothing * deltaTime);
            const glm::vec3 planar(velocity.x, 0.0f, velocity.z);
            const f32 rawSpeed = glm::length(planar);
            state.SmoothedSpeed += (rawSpeed - state.SmoothedSpeed) * alpha;

            f32 rawTurnRate = 0.0f;
            if (state.HasPrev && yawValid)
            {
                f32 yawDelta = yaw - state.PrevYaw;
                while (yawDelta > glm::pi<f32>())
                    yawDelta -= glm::two_pi<f32>();
                while (yawDelta < -glm::pi<f32>())
                    yawDelta += glm::two_pi<f32>();
                rawTurnRate = yawDelta / deltaTime;
            }
            state.SmoothedTurnRate += (rawTurnRate - state.SmoothedTurnRate) * alpha;

            state.PrevPosition = transform.Translation;
            state.PrevYaw = yaw;
            state.HasPrev = true;

            // ── Local movement direction, normalized for 2D blend spaces ─────
            const glm::vec3 local = glm::inverse(rotation) * planar;
            const f32 reference = std::max(loco.DirectionReferenceSpeed, 0.01f);
            const f32 moveX = glm::clamp(local.x / reference, -1.0f, 1.0f);
            const f32 moveY = glm::clamp(local.z / reference, -1.0f, 1.0f);

            // ── Gait hysteresis ───────────────────────────────────────────────
            state.CurrentGait = SelectGait(state.CurrentGait, state.SmoothedSpeed, loco);

            // ── Publish the parameters ────────────────────────────────────────
            WriteFloat(graphComp.Parameters, loco.SpeedParameter, state.SmoothedSpeed);
            WriteFloat(graphComp.Parameters, loco.DirectionXParameter, moveX);
            WriteFloat(graphComp.Parameters, loco.DirectionYParameter, moveY);
            WriteInt(graphComp.Parameters, loco.GaitParameter, state.CurrentGait);
            WriteFloat(graphComp.Parameters, loco.TurnParameter, state.SmoothedTurnRate);

            // ── Stride warp: scale the ACTIVE base-layer state's playback rate
            // so its authored stride speed matches the actual ground speed.
            // Root-motion characters get their movement from the clips (no
            // slide by construction); this is the velocity-driven fix.
            if (!graphComp.RuntimeGraph || graphComp.RuntimeGraph->Layers.empty())
            {
                continue;
            }
            auto& stateMachine = graphComp.RuntimeGraph->Layers[0].StateMachine;
            if (!stateMachine)
            {
                continue;
            }
            AnimationState* activeState = stateMachine->GetMutableState(stateMachine->GetCurrentStateName());
            if (!activeState)
            {
                continue;
            }

            // Remember the authored speed the first time the warp touches a state.
            auto [baseIt, inserted] = state.BaseStateSpeeds.try_emplace(activeState->Name, activeState->Speed);
            const f32 baseSpeed = baseIt->second;

            f32 strideScale = 1.0f;
            if (loco.StrideWarp && state.CurrentGait > 0)
            {
                const f32 clipSpeed = (state.CurrentGait == 2) ? loco.RunClipSpeed : loco.WalkClipSpeed;
                if (clipSpeed > 0.01f)
                {
                    const f32 maxScale = std::max(loco.MaxStrideScale, 1.0f);
                    strideScale = glm::clamp(state.SmoothedSpeed / clipSpeed, 1.0f / maxScale, maxScale);
                }
            }
            activeState->Speed = baseSpeed * strideScale;
        }
    }
} // namespace OloEngine::Animation

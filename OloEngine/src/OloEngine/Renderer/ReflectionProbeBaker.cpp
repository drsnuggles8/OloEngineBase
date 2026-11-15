#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ProbeSceneCapture.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/CaptureStateGuard.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    i32 SelectDominantReflectionProbe(const glm::vec3& cameraPosition,
                                      std::span<const ReflectionProbeRef> probes)
    {
        f32 bestDistSq = std::numeric_limits<f32>::max();
        i32 bestIndex = -1;
        for (sizet i = 0; i < probes.size(); ++i)
        {
            auto const& p = probes[i];
            glm::vec3 const delta = cameraPosition - p.Position;
            f32 const distSq = glm::dot(delta, delta);
            if (f32 const radiusSq = p.InfluenceRadius * p.InfluenceRadius; distSq > radiusSq)
            {
                continue;
            }
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestIndex = static_cast<i32>(i);
            }
        }
        return bestIndex;
    }

    Ref<TextureCubemap> ReflectionProbeBaker::CaptureSceneCubemap(Ref<Scene>& scene,
                                                                  const glm::vec3& position,
                                                                  u32 resolution,
                                                                  TArray<DDGIMeshCaster>* casterSink)
    {
        OLO_PROFILE_FUNCTION();

        // Defense in depth: BakeProbe already clamps, but this is private and
        // any future caller could pass 0 (GL undefined) or huge values (OOM).
        // Literal bounds match BakeProbe and SceneSerializer's deserialize clamp.
        u32 const clampedResolution = std::clamp(resolution, 16u, 2048u);
        if (clampedResolution != resolution)
        {
            OLO_CORE_WARN("ReflectionProbeBaker::CaptureSceneCubemap: resolution {} clamped to {}", resolution, clampedResolution);
        }

        CubemapSpecification cubemapSpec;
        cubemapSpec.Width = clampedResolution;
        cubemapSpec.Height = clampedResolution;
        cubemapSpec.Format = ImageFormat::RGBA32F;
        cubemapSpec.GenerateMips = true;
        auto cubemap = TextureCubemap::Create(cubemapSpec);
        if (!cubemap)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker: failed to allocate cubemap ({}x{} RGBA32F)", clampedResolution, clampedResolution);
            return nullptr;
        }

        // The scene's lit HDR radiance, face by face, through the main view's
        // render graph (ProbeSceneCapture), so the probe captures the same
        // lighting, sky and emissive surfaces as the live view.
        TArray<f32> faceTexels;
        bool const captured = ProbeSceneCapture::CaptureFaces(
            scene, position, clampedResolution, casterSink,
            [&cubemap, &faceTexels](u32 face, bool readOk, std::span<const f32> rgba)
            {
                if (!readOk)
                {
                    OLO_CORE_WARN("ReflectionProbeBaker: cubemap face readback failed");
                }
                // TextureCubemap::SetFaceData takes a mutable pointer. It
                // regenerates the mip chain on every call; redundant on faces
                // 0-4 but harmless, and a bake is an editor-time interactive
                // action.
                faceTexels.SetNum(rgba.size(), EAllowShrinking::No);
                std::copy(rgba.begin(), rgba.end(), faceTexels.GetData());
                cubemap->SetFaceData(face, faceTexels.GetData(), static_cast<u32>(rgba.size_bytes()));
                return true;
            });
        if (!captured)
        {
            return nullptr;
        }

        return cubemap;
    }

    Ref<ReflectionProbeDistanceField> ReflectionProbeBaker::CaptureDistanceField(std::span<const DDGIMeshCaster> casters,
                                                                                 const glm::vec3& position)
    {
        OLO_PROFILE_FUNCTION();

        // Per-draw UBO — GLSL twin: ProbeDistanceCaptureData (binding 7) in
        // ReflectionProbe_Distance.glsl.
        struct ProbeDistanceCaptureUBO
        {
            glm::mat4 Model;
            glm::vec4 ProbePosition;
        };

        constexpr u32 kRes = kProbeDistanceResolution;

        FramebufferSpecification spec;
        spec.Width = kRes;
        spec.Height = kRes;
        spec.Attachments = { FramebufferTextureFormat::RG32F, FramebufferTextureFormat::ShadowDepth };
        auto fbo = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/ReflectionProbe_Distance.glsl");
        if (!fbo || !shader)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker: distance-capture FBO or shader unavailable");
            return nullptr;
        }
        auto cameraUBO = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA, "CameraMatrices", "Camera");
        auto passUBO = UniformBuffer::Create(sizeof(ProbeDistanceCaptureUBO), ShaderBindingLayout::UBO_USER_0, "ProbeDistanceCaptureData");

        // Everything below runs in ABSOLUTE world space: the face cameras sit
        // at the world probe position and the caster transforms are absolute
        // (DDGIMeshCaster contract), so no render-origin shift is involved —
        // radial distances are origin-independent anyway.
        glm::mat4 const proj = glm::perspective(glm::radians(90.0f), 1.0f, kProbeDistanceNear, kProbeDistanceFar);

        TArray<f32> fieldData(static_cast<sizet>(kRes) * kRes * 6u, kProbeDistanceFar);
        TArray<f32> rgReadback(static_cast<sizet>(kRes) * kRes * 2u);
        sizet const faceBytes = rgReadback.Num() * sizeof(f32);

        // Restore on every exit path, including a throwing face capture: FBO,
        // cull flip, pre-capture viewport, dispatch state cache, and — because
        // this bake rewrote it — the engine camera UBO (see the guard's header).
        CaptureStateGuard stateGuard{ fbo, /*restoreCameraUBO=*/true };

        fbo->Bind();
        RenderCommand::SetViewport(0, 0, kRes, kRes);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::SetBlendState(false);
        // Backfaces must render: a probe looking at single-sided geometry
        // from behind still needs the surface's distance, not a see-through.
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);

        shader->Bind();
        cameraUBO->Bind();
        passUBO->Bind();

        RHI::ResourceHandle const colorAttachment = fbo->GetColorAttachmentHandle(0);
        bool readbackOk = true;
        for (u32 face = 0; face < 6; ++face)
        {
            // Un-covered texels keep the clear value == the miss sentinel of
            // the encoding contract (ReflectionProbeDistanceField.h).
            fbo->ClearAttachment(0, glm::vec4(kProbeDistanceFar, 0.0f, 0.0f, 0.0f));
            RenderCommand::ClearDepthOnly();

            glm::mat4 const view = glm::lookAt(position, position + ProbeSceneCapture::FaceTarget(face),
                                               ProbeSceneCapture::FaceUp(face));
            UBOStructures::CameraUBO camera{};
            // A8 seam, CAPTURE flavour (#691): these faces are read
            // back with ReadTextureImage and pushed into cube faces by
            // direction, the same shape as SkyCubemapBake / DDGI — the y flip
            // must NOT be applied or each face stores row-mirrored relative to
            // the GL bake. This writer predates the seam and uploaded the raw
            // matrix; identity on GL, so nothing changes there, and the path
            // is dormant on Vulkan until the cubemap CPU upload lands.
            camera.ViewProjection = RHI::AdjustCaptureProjectionForBackend(proj * view);
            camera.View = view;
            camera.Projection = RHI::AdjustCaptureProjectionForBackend(proj);
            camera.Position = position;
            camera.Pad0 = 0.0f;
            camera.PrevViewProjection = camera.ViewProjection;
            camera.RenderOrigin = glm::vec3(0.0f);
            // Capture flavour's reconstruction sibling = the raw matrix.
            camera.ProjectionForReconstruction = proj;
            cameraUBO->SetData(&camera, UBOStructures::CameraUBO::GetSize());

            for (auto const& caster : casters)
            {
                if (!caster.vaoID.IsValid() || caster.indexCount == 0)
                {
                    continue;
                }
                ProbeDistanceCaptureUBO data{};
                data.Model = caster.transform;
                data.ProbePosition = glm::vec4(position, 0.0f);
                passUBO->SetData(&data, sizeof(data));
                RenderCommand::DrawIndexedRaw(caster.vaoID, caster.indexCount, caster.baseIndex);
            }

            if (!RenderCommand::ReadTextureImage(colorAttachment, 0, RHI::Format::RG32Float,
                                                 faceBytes, rgReadback.GetData()))
            {
                OLO_CORE_WARN("ReflectionProbeBaker: distance face {} readback failed", face);
                readbackOk = false;
                break;
            }

            sizet const faceBase = static_cast<sizet>(face) * kRes * kRes;
            for (sizet i = 0; i < static_cast<sizet>(kRes) * kRes; ++i)
            {
                // Clamp into the contract range — geometry closer than the
                // near plane was clipped anyway, non-finite values must never
                // reach the field.
                f32 const distance = rgReadback[i * 2u];
                fieldData[faceBase + i] = std::isfinite(distance)
                                              ? std::clamp(distance, kProbeDistanceNear, kProbeDistanceFar)
                                              : kProbeDistanceFar;
            }
        }

        // stateGuard restores the FBO / cull / viewport / camera UBO on every
        // exit path, including the readback-failure return below.
        if (!readbackOk)
        {
            return nullptr;
        }
        return ReflectionProbeDistanceField::Create(std::move(fieldData), kRes);
    }

    bool ReflectionProbeBaker::BakeProbe(Ref<Scene>& scene,
                                         const glm::vec3& position,
                                         ReflectionProbeComponent& probe)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker::BakeProbe: scene is null");
            return false;
        }

        // Clamp resolution defensively — the component's serializer already
        // clamps loaded values, but in-memory edits may bypass that path.
        u32 const resolution = std::clamp(probe.m_Resolution, 16u, 2048u);

        // Exclude the probe being baked from the scene's reflection-probe IBL
        // override for the duration of the capture. CaptureSceneCubemap renders
        // the real scene through RenderScene3D, which calls
        // Scene::ApplyReflectionProbeOverride; on a RE-bake (the probe is
        // already active and carries a previous m_BakedEnvironment) that
        // override would select THIS probe's own stale environment as the
        // active IBL, feeding the prior bake back into the fresh capture — an
        // infinity-mirror feedback artifact that compounds every re-bake.
        // Clearing m_Active removes it from the override's candidate set (the
        // selection gate checks !m_Active first); other probes still contribute.
        // A scope guard restores the flag on EVERY exit — capture success,
        // capture failure, or an exception thrown from the capture.
        struct ActiveFlagGuard
        {
            bool& m_Flag;
            bool m_Previous;
            ~ActiveFlagGuard()
            {
                m_Flag = m_Previous;
            }
        } activeGuard{ probe.m_Active, probe.m_Active };
        probe.m_Active = false;

        TArray<DDGIMeshCaster> casters;
        auto cubemap = CaptureSceneCubemap(scene, position, resolution, &casters);
        if (!cubemap)
        {
            return false;
        }

        // Hand off to EnvironmentMap which runs the IBL chain (irradiance +
        // prefiltered specular + BRDF LUT). The same plumbing the global
        // EnvironmentMapComponent uses — Renderer3D::SetGlobalIBL can consume
        // it without modification.
        //
        // Opt OUT of the IBL disk cache. A captured probe cubemap is a render
        // target whose pixels depend on the scene, probe position and time, but
        // it carries a constant debug name ("Generated Cubemap"), so the cache
        // key (name + dimensions) collides across every bake at the same
        // resolution. A cache hit would serve a PREVIOUS bake's IBL for freshly
        // captured pixels — re-baking, baking a second probe, or baking after a
        // scene change would all silently reuse stale (even all-black) maps.
        // Procedural/StarNest skies opt out for the identical reason.
        IBLConfiguration iblConfig;
        iblConfig.UseDiskCache = false;
        auto environment = EnvironmentMap::CreateFromCubemap(cubemap, iblConfig);
        if (!environment || !environment->HasIBL())
        {
            OLO_CORE_ERROR("ReflectionProbeBaker::BakeProbe: IBL generation failed");
            return false;
        }

        // Distance-impostor capture (issue #705): rasterize the casters the
        // warm-up render collected into the radial-distance field. A probe
        // with no distance field still works — the per-pixel probe path just
        // skips it and the global IBL fallback covers those pixels — so a
        // failed distance capture degrades rather than failing the bake.
        if (auto distanceField = CaptureDistanceField({ casters.GetData(), static_cast<sizet>(casters.Num()) }, position))
        {
            environment->SetProbeDistanceField(distanceField);
        }
        else
        {
            OLO_CORE_WARN("ReflectionProbeBaker: distance capture failed — probe will not parallax-correct");
        }

        probe.m_BakedEnvironment = environment;
        probe.m_NeedsBake = false;

        OLO_CORE_INFO("Baked reflection probe at ({}, {}, {}) — cubemap {}x{}, {} casters in distance field",
                      position.x, position.y, position.z, resolution, resolution, casters.Num());
        return true;
    }
} // namespace OloEngine

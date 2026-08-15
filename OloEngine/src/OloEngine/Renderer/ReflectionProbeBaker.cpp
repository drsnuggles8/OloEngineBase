#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/ReflectionProbeDistanceField.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
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
#include <vector>

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

    namespace
    {
        // Cubemap face look directions and up vectors. Order matches
        // GL_TEXTURE_CUBE_MAP_POSITIVE_X .. NEGATIVE_Z and the bindings used in
        // IBLPrecompute::RenderToCubemap, so prefilter / irradiance generation
        // see the same orientation as our captured faces.
        constexpr glm::vec3 s_FaceTargets[6] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
        };

        constexpr glm::vec3 s_FaceUps[6] = {
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
        };
    } // namespace

    Ref<TextureCubemap> ReflectionProbeBaker::CaptureSceneCubemap(Ref<Scene>& scene,
                                                                  const glm::vec3& position,
                                                                  u32 resolution,
                                                                  std::vector<DDGIMeshCaster>* casterSink)
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

        // The scene is rendered through the full Renderer3D render graph so the
        // probe captures the same lighting / skybox / emissive surfaces as the
        // live view. The graph writes into its OWN targets — never an
        // externally-bound framebuffer — so we resize the graph to the probe
        // resolution, render each face, and read back the graph's HDR SceneColor
        // (the MRT-root scene-pass output: linear radiance BEFORE tone mapping
        // and post/TAA, which is exactly what the IBL convolution wants).
        //
        // The previous approach bound a local capture FBO and read it back; that
        // FBO was never a render target (RenderScene3D targets the graph), so the
        // bake captured only the cleared (black) FBO — every probe baked black.

        // Allocate the readback buffer up front — 4 floats per pixel. The
        // destination cubemap face is RGBA32F; SceneColor RT0 is read back as
        // RGBA float regardless of its own internal format. Doing this BEFORE we
        // mutate any global render state (size / scale below) keeps the only
        // realistic throw here (bad_alloc at high resolution) from stranding the
        // live view at the probe's size/scale.
        sizet const faceBytes = static_cast<sizet>(clampedResolution) * clampedResolution * 4u * sizeof(f32);
        std::vector<f32> pixelBuffer(faceBytes / sizeof(f32));

        // Snapshot the live render size so we can restore it afterwards. In the
        // editor the SceneColor target already exists (the viewport has been
        // rendering), so this captures the viewport size. On a first bake before
        // any frame has rendered (e.g. tests that bake before their first
        // RunEditorFrames) the target isn't allocated yet — that's fine: the
        // warm-up render below creates it, there's simply no prior size to
        // restore, and the caller re-asserts its own size after the bake.
        u32 savedWidth = 0;
        u32 savedHeight = 0;
        if (auto const preBakeSceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor))
        {
            savedWidth = preBakeSceneFb->GetSpecification().Width;
            savedHeight = preBakeSceneFb->GetSpecification().Height;
        }

        // Force render scale to 1.0 for the capture. With Dynamic Resolution
        // Scaling active the scene is rendered into a floor(width*scale)
        // sub-viewport of the target, but we read back the FULL physical
        // texture — so a scale < 1.0 would capture the scene in one corner
        // surrounded by un-rendered pixels and bake a corrupt environment.
        // Restore the user's scale afterward.
        f32 const savedRenderScale = Renderer3D::GetRenderScale();
        Renderer3D::SetRenderScale(1.0f);

        // Square render targets so the 90° FOV per face covers the whole image.
        Renderer3D::OnWindowResize(clampedResolution, clampedResolution);

        // 90° FOV per face — 6 faces cover the full sphere with no overlap.
        // Far plane is generous; near plane small enough to capture nearby
        // walls when the probe sits inside an enclosed room.
        Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

        // Prime the render graph at the new size before capturing: the first
        // render(s) after a graph resize come back black (the graph needs a
        // frame to settle into the reallocated targets). Without this every
        // captured face is black even though RenderScene3D works fine at the
        // unchanged live-view size.
        //
        // The warm-up render doubles as the caster-collection pass for the
        // distance capture: the scene's mesh submission is registry-driven
        // (not frustum-culled on the CPU), so ONE render enumerates every
        // opaque caster regardless of which face the camera looks at. The
        // guard clears the sink pointer on every exit so a throw inside
        // RenderScene3D can never leave Renderer3D pointing at a dead vector.
        {
            struct AuxSinkGuard
            {
                ~AuxSinkGuard()
                {
                    Renderer3D::SetAuxCasterSink(nullptr);
                }
            } sinkGuard;
            Renderer3D::SetAuxCasterSink(casterSink);

            glm::mat4 const warmView = glm::inverse(glm::lookAt(position, position + s_FaceTargets[0], s_FaceUps[0]));
            scene->RenderScene3D(captureCamera, warmView);
        }

        // Resolve the (now resized) capture target once. It is stable across all
        // six same-size face renders — RenderScene3D does not reallocate it at a
        // constant size — so there's no need to re-resolve per face.
        auto const sceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
        bool const captureOk = (sceneFb != nullptr);
        if (!sceneFb)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker: SceneColor framebuffer unavailable after graph resize");
        }
        else
        {
            // Hoisted outside the face loop, as the native id was: SceneColor's
            // attachment is not recreated between faces, and the handle stays
            // valid across an in-place recreate (issue #691 step 3).
            RHI::ResourceHandle const colorAttachment = sceneFb->GetColorAttachmentHandle(0);
            for (u32 face = 0; face < 6; ++face)
            {
                glm::mat4 const view = glm::lookAt(position, position + s_FaceTargets[face], s_FaceUps[face]);
                glm::mat4 const transform = glm::inverse(view);

                scene->RenderScene3D(captureCamera, transform);

                // Read back this face's lit HDR radiance from the graph's
                // SceneColor RT0. The readback reads the texture directly
                // (no FBO-bound restriction) and lets the driver pick its path.
                if (!RenderCommand::ReadTextureImage(colorAttachment, 0, RHI::Format::RGBA32Float,
                                                     faceBytes, pixelBuffer.data()))
                {
                    OLO_CORE_WARN("ReflectionProbeBaker: cubemap face readback failed");
                }

                // Upload into the cubemap face. SetFaceData triggers
                // glGenerateTextureMipmap on every call; redundant on faces
                // 0-4 but harmless and keeps the API surface small. Bake is an
                // editor-time interactive action so the cost is acceptable.
                cubemap->SetFaceData(face, pixelBuffer.data(), static_cast<u32>(faceBytes));
            }
        }

        // Restore the live-view render scale and size (the bake forced scale to
        // 1.0 and squashed the graph down to the probe resolution above). Always
        // restore the scale; restore the size only if we snapshotted one (a
        // first bake before any render has no prior size — the caller re-asserts
        // it). Both run on every non-throwing exit, capture success or failure.
        Renderer3D::SetRenderScale(savedRenderScale);
        if (savedWidth != 0 && savedHeight != 0)
        {
            Renderer3D::OnWindowResize(savedWidth, savedHeight);
        }

        if (!captureOk)
        {
            return nullptr;
        }

        return cubemap;
    }

    Ref<ReflectionProbeDistanceField> ReflectionProbeBaker::CaptureDistanceField(const std::vector<DDGIMeshCaster>& casters,
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
        auto cameraUBO = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        auto passUBO = UniformBuffer::Create(sizeof(ProbeDistanceCaptureUBO), ShaderBindingLayout::UBO_USER_0);

        // Everything below runs in ABSOLUTE world space: the face cameras sit
        // at the world probe position and the caster transforms are absolute
        // (DDGIMeshCaster contract), so no render-origin shift is involved —
        // radial distances are origin-independent anyway.
        glm::mat4 const proj = glm::perspective(glm::radians(90.0f), 1.0f, kProbeDistanceNear, kProbeDistanceFar);

        std::vector<f32> fieldData(static_cast<sizet>(kRes) * kRes * 6u, kProbeDistanceFar);
        std::vector<f32> rgReadback(static_cast<sizet>(kRes) * kRes * 2u);
        sizet const faceBytes = rgReadback.size() * sizeof(f32);

        // Deliberate restore as a scope guard (no GLStateGuard here — see
        // docs/agent-rules/render-pass-published-state.md) so it also runs if
        // a face capture throws: unbind the FBO, undo the cull flip, put the
        // pre-capture viewport back, and re-establish the engine camera UBO.
        // The bind cache must be invalidated FIRST or BindUBOIfNeeded thinks
        // the camera binding never changed and skips the re-bind.
        struct CaptureStateGuard
        {
            Ref<Framebuffer>& m_Fbo;
            Viewport m_PrevViewport;
            ~CaptureStateGuard()
            {
                m_Fbo->Unbind();
                RenderCommand::EnableCulling();
                RenderCommand::SetViewport(m_PrevViewport.x, m_PrevViewport.y,
                                           m_PrevViewport.width, m_PrevViewport.height);
                CommandDispatch::InvalidateRenderStateCache();
                CommandDispatch::UploadCameraUBO();
            }
        } stateGuard{ fbo, RenderCommand::GetViewport() };

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

            glm::mat4 const view = glm::lookAt(position, position + s_FaceTargets[face], s_FaceUps[face]);
            UBOStructures::CameraUBO camera{};
            // A8 seam, CAPTURE flavour (#691 Phase 8): these faces are read
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
            camera._padding0 = 0.0f;
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
                                                 faceBytes, rgReadback.data()))
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

        std::vector<DDGIMeshCaster> casters;
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
        if (auto distanceField = CaptureDistanceField(casters, position))
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
                      position.x, position.y, position.z, resolution, resolution, casters.size());
        return true;
    }
} // namespace OloEngine

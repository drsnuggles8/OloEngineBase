#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ProbeSceneCapture.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>

namespace OloEngine
{
    namespace
    {
        constexpr std::array<glm::vec3, ProbeSceneCapture::kFaceCount> kFaceTargets{
            glm::vec3{ 1.0f, 0.0f, 0.0f },
            glm::vec3{ -1.0f, 0.0f, 0.0f },
            glm::vec3{ 0.0f, 1.0f, 0.0f },
            glm::vec3{ 0.0f, -1.0f, 0.0f },
            glm::vec3{ 0.0f, 0.0f, 1.0f },
            glm::vec3{ 0.0f, 0.0f, -1.0f },
        };

        constexpr std::array<glm::vec3, ProbeSceneCapture::kFaceCount> kFaceUps{
            glm::vec3{ 0.0f, -1.0f, 0.0f },
            glm::vec3{ 0.0f, -1.0f, 0.0f },
            glm::vec3{ 0.0f, 0.0f, 1.0f },
            glm::vec3{ 0.0f, 0.0f, -1.0f },
            glm::vec3{ 0.0f, -1.0f, 0.0f },
            glm::vec3{ 0.0f, -1.0f, 0.0f },
        };

        // The two settings of the main view a capture cannot render under, off
        // for its duration and back on every exit.
        //
        // * A lighting tap (#1526) is a debug view of the MAIN view; the capture
        //   renders through the main camera path, so a bake taken while one is
        //   selected stored that one term as the environment.
        // * An upscale preset renders the scene band BELOW the view's size: the
        //   pipeline resizes the SceneColor target to floor(size * scale) every
        //   frame, whatever Renderer3D::SetRenderScale says. The capture reads
        //   that target back as a full face, so a bake taken with an upscaler
        //   on read a smaller image into the start of each face and left the
        //   rest of it whatever the buffer held (#1352).
        struct ScopedCaptureSettings
        {
            LightingTap SavedTap = Renderer3D::GetPostProcessSettings().LightingDebugTap;
            UpscaleMode SavedUpscale = Renderer3D::GetPostProcessSettings().Upscale;
            ScopedCaptureSettings()
            {
                Renderer3D::GetPostProcessSettings().LightingDebugTap = LightingTap::None;
                Renderer3D::GetPostProcessSettings().Upscale = UpscaleMode::Off;
            }
            ~ScopedCaptureSettings()
            {
                Renderer3D::GetPostProcessSettings().LightingDebugTap = SavedTap;
                Renderer3D::GetPostProcessSettings().Upscale = SavedUpscale;
            }
            ScopedCaptureSettings(const ScopedCaptureSettings&) = delete;
            ScopedCaptureSettings& operator=(const ScopedCaptureSettings&) = delete;
        };

        // Clears the sink pointer on every exit, so a throw inside
        // RenderScene3D cannot leave Renderer3D pointing at a dead array.
        struct AuxSinkGuard
        {
            AuxSinkGuard() = default;
            ~AuxSinkGuard()
            {
                Renderer3D::SetAuxCasterSink(nullptr);
            }
            AuxSinkGuard(const AuxSinkGuard&) = delete;
            AuxSinkGuard& operator=(const AuxSinkGuard&) = delete;
        };

        // The main view's size and render scale, put back on EVERY exit: a
        // throw out of RenderScene3D or the face sink must not strand the live
        // view at the probe's size and scale 1.
        //
        // The size is the view's configured one, not a target's: on a first
        // capture before any frame has rendered the SceneColor target does not
        // exist yet, and a size read from it was zero, so nothing was put back.
        struct ScopedMainViewSize
        {
            RenderViewInfo Saved = Renderer3D::GetViewInfo(kMainRenderView);
            f32 SavedRenderScale = Renderer3D::GetRenderScale();
            ScopedMainViewSize() = default;
            ~ScopedMainViewSize()
            {
                Renderer3D::SetRenderScale(SavedRenderScale);
                if (Saved.Width != 0 && Saved.Height != 0)
                {
                    Renderer3D::OnWindowResize(Saved.Width, Saved.Height);
                }
            }
            ScopedMainViewSize(const ScopedMainViewSize&) = delete;
            ScopedMainViewSize& operator=(const ScopedMainViewSize&) = delete;
        };
    } // namespace

    void ProbeSceneCapture::NormalizeFaceRows(std::span<f32> rgba, u32 resolution, bool rowsAreBottomUp)
    {
        const sizet rowFloats = static_cast<sizet>(resolution) * 4u;
        if (rowsAreBottomUp || resolution < 2u || rgba.size() != rowFloats * resolution)
        {
            return;
        }
        f32* const texels = rgba.data();
        for (u32 top = 0, bottom = resolution - 1u; top < bottom; ++top, --bottom)
        {
            std::swap_ranges(texels + static_cast<sizet>(top) * rowFloats, texels + (static_cast<sizet>(top) + 1u) * rowFloats,
                             texels + static_cast<sizet>(bottom) * rowFloats);
        }
    }

    glm::vec3 ProbeSceneCapture::FaceTarget(u32 face)
    {
        return kFaceTargets[face % kFaceCount];
    }

    glm::vec3 ProbeSceneCapture::FaceUp(u32 face)
    {
        return kFaceUps[face % kFaceCount];
    }

    glm::mat4 ProbeSceneCapture::FaceTransform(const glm::vec3& position, u32 face)
    {
        return glm::inverse(glm::lookAt(position, position + FaceTarget(face), FaceUp(face)));
    }

    bool ProbeSceneCapture::CaptureFaces(Ref<Scene>& scene, const glm::vec3& position, u32 resolution,
                                         TArray<DDGIMeshCaster>* casterSink, const FaceSink& onFace, bool submitEachFace)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene || !onFace)
        {
            OLO_CORE_ERROR("ProbeSceneCapture::CaptureFaces: {}", !scene ? "scene is null" : "no face sink");
            return false;
        }
        const bool explicitSubmit = submitEachFace && RendererAPI::GetAPI() == RendererAPI::API::Vulkan;
        if (explicitSubmit && !RenderCommand::SupportsRenderGraphFenceSubmission())
        {
            OLO_CORE_ERROR("ProbeSceneCapture: Vulkan capture requires a frame recording");
            return false;
        }

        // 0 is undefined to the device and a huge value is an allocation
        // failure. The bounds are the reflection probe component's.
        u32 const clampedResolution = std::clamp(resolution, 16u, 2048u);
        if (clampedResolution != resolution)
        {
            OLO_CORE_WARN("ProbeSceneCapture::CaptureFaces: resolution {} clamped to {}", resolution, clampedResolution);
        }

        // Allocated BEFORE any render state changes below: the only realistic
        // throw here (bad_alloc at a high resolution) must not strand the live
        // view at the probe's size and scale.
        sizet const faceBytes = static_cast<sizet>(clampedResolution) * clampedResolution * 4u * sizeof(f32);
        TArray<f32> pixelBuffer(faceBytes / sizeof(f32));

        // A capture renders through the MAIN view, whichever view its caller
        // had active (ADR 0026, section 6): the main view's per-view
        // accumulations (DDGI cascades, the virtual shadow map's page cache)
        // are the converged ones, and it is the main view's size that is put
        // back.
        const Renderer3D::ScopedView mainView(kMainRenderView);

        // The live size and render scale, restored when this leaves scope
        // (declared after `mainView`, so while the main view is still active).
        const ScopedMainViewSize restoreMainViewSize;

        // Render scale 1 for the capture. With dynamic resolution active the
        // scene is rendered into a floor(width * scale) sub-viewport of the
        // target, and the FULL physical texture is read back: a scale below 1
        // would capture the scene in one corner surrounded by unrendered texels.
        Renderer3D::SetRenderScale(1.0f);

        // Every off-screen target stores its rows bottom-up on OpenGL and
        // top-down on Vulkan (RHI::RenderTargetRowsAreBottomUp). The face
        // orientations are chosen for the bottom-up order, and both bakers
        // consume the texels by position (a cube map face upload, a spherical
        // harmonics projection that maps a row to a direction), so a top-down
        // readback handed over as it came stored every side face upside down:
        // light from above and below swapped in whatever was baked on Vulkan.
        bool const rowsAreBottomUp = RHI::RenderTargetRowsAreBottomUp();

        bool captured = false;
        {
            const ScopedCaptureSettings captureSettings;

            // Square targets, so the 90 degree field of view per face covers
            // the whole image.
            Renderer3D::OnWindowResize(clampedResolution, clampedResolution);

            // Six 90 degree faces cover the sphere with no overlap. The far
            // plane is generous; the near plane is small enough to capture
            // nearby walls when the probe sits inside a room.
            Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

            // Prime the graph at the new size: the first render after a resize
            // comes back black (the graph needs a frame to settle into the
            // reallocated targets).
            //
            // The warm-up render doubles as the caster-collection pass: mesh
            // submission is registry-driven, not frustum-culled on the CPU, so
            // ONE render enumerates every opaque caster whichever face the
            // camera looks along.
            {
                const AuxSinkGuard sinkGuard;
                Renderer3D::SetAuxCasterSink(casterSink);
                scene->RenderScene3D(captureCamera, FaceTransform(position, 0u));
            }
            if (explicitSubmit && !SubmitFaceForReadback())
                return false;

            // Resolved once: the target is stable across six same-size renders.
            if (auto const sceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); !sceneFb)
            {
                OLO_CORE_ERROR("ProbeSceneCapture: SceneColor framebuffer unavailable after the graph resize");
            }
            else if (sceneFb->GetSpecification().Width != clampedResolution || sceneFb->GetSpecification().Height != clampedResolution)
            {
                // A face is read back as clampedResolution squared texels. A
                // target of any other size fills only part of it, and the rest
                // would be baked as it lies: refuse instead.
                OLO_CORE_ERROR("ProbeSceneCapture: the SceneColor target is {} x {}, not the {} x {} a face is read as; "
                               "something renders the main view's scene band at another size",
                               sceneFb->GetSpecification().Width, sceneFb->GetSpecification().Height, clampedResolution,
                               clampedResolution);
            }
            else
            {
                // The handle stays valid across an in-place recreate (#691).
                RHI::ResourceHandle const colorAttachment = sceneFb->GetColorAttachmentHandle(0);
                captured = true;
                for (u32 face = 0; face < kFaceCount; ++face)
                {
                    scene->RenderScene3D(captureCamera, FaceTransform(position, face));

                    if (explicitSubmit && !SubmitFaceForReadback())
                    {
                        captured = false;
                        break;
                    }

                    // The face's lit HDR radiance, read from the texture
                    // directly (no framebuffer-bound restriction).
                    bool const readOk = RenderCommand::ReadTextureImage(colorAttachment, 0, RHI::Format::RGBA32Float,
                                                                        faceBytes, pixelBuffer.GetData());
                    if (readOk)
                    {
                        NormalizeFaceRows(std::span<f32>(pixelBuffer.GetData(), static_cast<sizet>(pixelBuffer.Num())),
                                          clampedResolution, rowsAreBottomUp);
                    }
                    if (!onFace(face, readOk, std::span<const f32>(pixelBuffer.GetData(), static_cast<sizet>(pixelBuffer.Num()))))
                    {
                        captured = false;
                        break;
                    }
                }
            }
        }

        return captured;
    }

    bool ProbeSceneCapture::SubmitFaceForReadback()
    {
        if (RendererAPI::GetAPI() != RendererAPI::API::Vulkan || RenderCommand::SubmitRenderGraphFenceSegment())
            return true;

        OLO_CORE_ERROR("ProbeSceneCapture: Vulkan cannot submit the face from this recording point");
        return false;
    }
} // namespace OloEngine

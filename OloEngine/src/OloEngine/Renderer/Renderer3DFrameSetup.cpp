#include "OloEnginePCH.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"

namespace OloEngine
{
    void Renderer3D::ObserveTemporalProjection(const glm::mat4& projection)
    {
        if (s_Data.HasTemporalProjectionMatrix && !Math::BitwiseEqual(s_Data.TemporalProjectionMatrix, projection))
            InvalidateTemporalHistories(TemporalHistoryInvalidationCause::ProjectionChanged);
        s_Data.TemporalProjectionMatrix = projection;
        s_Data.HasTemporalProjectionMatrix = true;
    }

    void Renderer3D::RefreshCullingCamera()
    {
        // Reconcile with the settings bool FIRST, every frame. The Renderer
        // Settings panel, the F3 overlay and the MCP tool all write that bool,
        // and only some of those paths call ApplyRendererSettings() -- honouring
        // it here means a toggle takes effect on the next frame regardless of
        // who flipped it, including in a shipped runtime build where the F3
        // overlay is the only UI there is. SetCullingCameraFrozen is the
        // authority (it owns the snapshot) and writes the bool back, so the two
        // cannot drift.
        if (s_Data.Settings.ObserverCameraEnabled != s_Data.CullingCameraFrozen)
            SetCullingCameraFrozen(s_Data.Settings.ObserverCameraEnabled);

        // Frozen: leave every Cull* field exactly as SetCullingCameraFrozen(true)
        // captured it. That single early-out is the whole feature — every
        // culling / LOD / Hi-Z consumer below reads these fields, so nothing
        // needs a per-site "if frozen" branch, and a site that forgot to switch
        // over is a site that keeps following the observer rather than one that
        // silently half-freezes.
        if (s_Data.CullingCameraFrozen)
            return;

        s_Data.CullViewMatrix = s_Data.ViewMatrix;
        s_Data.CullProjectionMatrix = s_Data.ProjectionMatrix;
        s_Data.CullViewProjectionMatrix = s_Data.ViewProjectionMatrix;
        s_Data.CullViewPos = s_Data.ViewPos;
        s_Data.CullNearClip = s_Data.CameraNearClip;
        s_Data.CullFarClip = s_Data.CameraFarClip;
    }

    void Renderer3D::BeginScene(const PerspectiveCamera& camera)
    {
        OLO_PROFILE_FUNCTION();
        AdvanceDecalVisibilityFrame();
        // Ray-traced shadow candidates are per-frame (issue #1056). Cleared
        // HERE so an empty list can only mean "no light asked this frame" —
        // a scene that stops publishing them must not keep the last frame's.
        s_Data.RayTracedShadowLightRequests.clear();

        const glm::mat4 projection = camera.GetProjection();
        ObserveTemporalProjection(projection);
        s_Data.ViewMatrix = camera.GetView();
        s_Data.ProjectionMatrix = projection;
        s_Data.ViewProjectionMatrix = camera.GetViewProjection();
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        RefreshCullingCamera();

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::BeginScene(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();
        AdvanceDecalVisibilityFrame();
        // Ray-traced shadow candidates are per-frame (issue #1056). Cleared
        // HERE so an empty list can only mean "no light asked this frame" —
        // a scene that stops publishing them must not keep the last frame's.
        s_Data.RayTracedShadowLightRequests.clear();

        const glm::mat4 projection = camera.GetProjection();
        ObserveTemporalProjection(projection);
        s_Data.ViewMatrix = camera.GetViewMatrix();
        s_Data.ProjectionMatrix = projection;
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        RefreshCullingCamera();

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();
        AdvanceDecalVisibilityFrame();
        // Ray-traced shadow candidates are per-frame (issue #1056). Cleared
        // HERE so an empty list can only mean "no light asked this frame" —
        // a scene that stops publishing them must not keep the last frame's.
        s_Data.RayTracedShadowLightRequests.clear();

        const glm::mat4 projection = camera.GetProjection();
        ObserveTemporalProjection(projection);
        s_Data.ViewMatrix = glm::inverse(transform);
        s_Data.ProjectionMatrix = projection;
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = glm::vec3(transform[3]);
        // Camera base class has no near/far — keep previous values

        RefreshCullingCamera();

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::UploadFogVolumes(const FogVolumesUBOData& data)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.SceneEffectsGPU.FogVolumesData = data;
    }
} // namespace OloEngine

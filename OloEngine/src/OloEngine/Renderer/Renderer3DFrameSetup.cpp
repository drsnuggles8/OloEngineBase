#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"

namespace OloEngine
{
    void Renderer3D::BeginScene(const PerspectiveCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetView();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = camera.GetViewProjection();
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::BeginScene(const EditorCamera& camera)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = camera.GetViewMatrix();
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = camera.GetPosition();
        s_Data.CameraNearClip = camera.GetNearClip();
        s_Data.CameraFarClip = camera.GetFarClip();

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.ViewMatrix = glm::inverse(transform);
        s_Data.ProjectionMatrix = camera.GetProjection();
        s_Data.ViewProjectionMatrix = s_Data.ProjectionMatrix * s_Data.ViewMatrix;
        s_Data.ViewPos = glm::vec3(transform[3]);
        // Camera base class has no near/far — keep previous values

        s_Data.Pipeline->PrepareFrame(s_Data, m_ShaderLibrary);
    }

    void Renderer3D::UploadFogVolumes(const FogVolumesUBOData& data)
    {
        OLO_PROFILE_FUNCTION();

        s_Data.SceneEffectsGPU.FogVolumesData = data;
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <utility>
#include <variant>

namespace OloEngine
{
    void Renderer3D::UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection)
    {
        OLO_PROFILE_FUNCTION();

        ShaderBindingLayout::CameraUBO cameraData;
        cameraData.ViewProjection = projection * view;
        cameraData.View = view;
        cameraData.Projection = projection;
        cameraData.Position = s_Data.ViewPos;
        cameraData._padding0 = 0.0f;
        // Previous-frame view-projection is maintained by BeginSceneCommon
        // in `s_Data.PrevViewProjectionMatrix`. Forward PBR shaders consume
        // this through the CameraMatrices UBO (binding 0) to emit screen-
        // space velocity into scene FB RT3 — mirroring what the deferred
        // G-Buffer PBR shader does through u_PrevViewProjection.
        cameraData.PrevViewProjection = s_Data.PrevViewProjectionMatrix;

        constexpr auto expectedSize = ShaderBindingLayout::CameraUBO::GetSize();
        static_assert(sizeof(ShaderBindingLayout::CameraUBO) == expectedSize, "CameraUBO size mismatch");

        s_Data.SharedSceneUBOs.Camera->SetData(&cameraData, expectedSize);

        // Re-bind to ensure this UBO is active at binding point 0.
        // Other subsystems (e.g. ShadowMap) create their own camera UBOs at the
        // same binding point, which can overwrite the persistent binding.
        s_Data.SharedSceneUBOs.Camera->Bind();
    }

    void Renderer3D::UpdateLightPropertiesUBO()
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Data.SharedSceneUBOs.LightProperties)
        {
            return;
        }

        ShaderBindingLayout::LightUBO lightData;
        const auto lightType = std::to_underlying(s_Data.SceneLight.Type);

        lightData.LightPosition = glm::vec4(s_Data.SceneLight.Position, 1.0f);
        lightData.LightDirection = glm::vec4(s_Data.SceneLight.Direction, 0.0f);
        lightData.LightAmbient = glm::vec4(s_Data.SceneLight.Ambient, 0.0f);
        lightData.LightDiffuse = glm::vec4(s_Data.SceneLight.Diffuse, 0.0f);
        lightData.LightSpecular = glm::vec4(s_Data.SceneLight.Specular, 0.0f);
        lightData.LightAttParams = glm::vec4(
            s_Data.SceneLight.Constant,
            s_Data.SceneLight.Linear,
            s_Data.SceneLight.Quadratic,
            0.0f);
        lightData.LightSpotParams = glm::vec4(
            s_Data.SceneLight.CutOff,
            s_Data.SceneLight.OuterCutOff,
            0.0f,
            0.0f);
        lightData.ViewPosAndLightType = glm::vec4(s_Data.ViewPos, static_cast<f32>(lightType));

        s_Data.SharedSceneUBOs.LightProperties->SetData(&lightData, sizeof(ShaderBindingLayout::LightUBO));
    }

    void Renderer3D::SetRenderScale(f32 scale)
    {
        OLO_PROFILE_FUNCTION();
        if (!s_Data.RGraph)
        {
            return;
        }

        s_Data.RGraph->SetRenderScale(scale);

        // Upload immediately so the DRS UBO reflects the new scale even if called
        // outside BeginSceneCommon (e.g. from the settings panel).
        const glm::vec2 bounds = s_Data.RGraph->GetRenderScaleBounds();
        s_Data.SceneEffectsGPU.DRSData.RenderScaleBounds = bounds;
        if (s_Data.SceneEffectsGPU.DRS)
        {
            s_Data.SceneEffectsGPU.DRS->SetData(&s_Data.SceneEffectsGPU.DRSData, DRSUBOData::GetSize());
        }
    }

    void Renderer3D::BindSceneUBOs()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.SharedSceneUBOs.Camera)
        {
            s_Data.SharedSceneUBOs.Camera->Bind();
        }

        if (s_Data.SharedSceneUBOs.LightProperties)
        {
            s_Data.SharedSceneUBOs.LightProperties->Bind();
        }

        // Ensure Forward+ UBO is always bound (with Enabled=0 when inactive)
        // so fragment shaders can always read fplus_Params.z
        s_Data.ForwardPlus.UploadDisabledUBO();
    }

    void Renderer3D::ApplyGlobalResources()
    {
        OLO_PROFILE_FUNCTION();

        const auto& shaderRegistries = s_Data.ShaderRegistries;
        const auto& globalResources = s_Data.GlobalResourceRegistry.GetBoundResources();

        for (const auto& shaderRegistryEntry : shaderRegistries)
        {
            auto* registry = shaderRegistryEntry.second;
            if (!registry)
            {
                continue;
            }

            for (const auto& [resourceName, resource] : globalResources)
            {
                if (registry->GetBindingInfo(resourceName) == nullptr)
                {
                    continue;
                }

                ShaderResourceInput input;
                if (std::holds_alternative<Ref<UniformBuffer>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<UniformBuffer>>(resource));
                }
                else if (std::holds_alternative<Ref<Texture2D>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<Texture2D>>(resource));
                }
                else if (std::holds_alternative<Ref<TextureCubemap>>(resource))
                {
                    input = ShaderResourceInput(std::get<Ref<TextureCubemap>>(resource));
                }

                if (input.Type != ShaderResourceType::None)
                {
                    registry->SetResource(resourceName, input);
                }
            }
        }
    }

    ShaderResourceRegistry* Renderer3D::GetShaderRegistry(u32 shaderID)
    {
        if (auto it = s_Data.ShaderRegistries.find(shaderID); it != s_Data.ShaderRegistries.end())
        {
            return it->second;
        }

        return nullptr;
    }

    void Renderer3D::RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry)
    {
        if (!registry)
        {
            return;
        }

        s_Data.ShaderRegistries[shaderID] = registry;
        OLO_CORE_TRACE("Renderer3D: Registered shader registry for shader ID: {0}", shaderID);
    }

    void Renderer3D::UnregisterShaderRegistry(u32 shaderID)
    {
        if (auto it = s_Data.ShaderRegistries.find(shaderID); it != s_Data.ShaderRegistries.end())
        {
            s_Data.ShaderRegistries.erase(it);
            OLO_CORE_TRACE("Renderer3D: Unregistered shader registry for shader ID: {0}", shaderID);
        }
    }

    const std::unordered_map<u32, ShaderResourceRegistry*>& Renderer3D::GetShaderRegistries()
    {
        return s_Data.ShaderRegistries;
    }

    void Renderer3D::ApplyResourceBindings(u32 shaderID)
    {
        if (auto* registry = GetShaderRegistry(shaderID))
        {
            registry->ApplyBindings();
        }
    }

    ShaderLibrary& Renderer3D::GetShaderLibrary()
    {
        return m_ShaderLibrary;
    }
} // namespace OloEngine
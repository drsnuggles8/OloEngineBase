#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IMaterial.h"
#include "OloEngine/Renderer/PBRMaterial.h"
#include "OloEngine/Renderer/PhongMaterial.h"

namespace OloEngine
{
    Ref<IMaterial> MaterialFactory::Create(MaterialType type)
    {
        switch (type)
        {
            case MaterialType::PBR:
                return CreateRef<PBRMaterial>();
            case MaterialType::Phong:
                return CreateRef<PhongMaterial>();
            default:
                OLO_CORE_ERROR("MaterialFactory::Create: Unknown material type");
                return nullptr;
        }
    }

    Ref<IMaterial> MaterialFactory::CreatePBRMaterial(
        const glm::vec3& baseColor,
        float metallic,
        float roughness)
    {
        auto material = CreateRef<PBRMaterial>();
        material->SetBaseColor(baseColor);
        material->SetMetallicRoughness(metallic, roughness);
        return material;
    }

    Ref<IMaterial> MaterialFactory::CreatePhongMaterial(
        const glm::vec3& ambient,
        const glm::vec3& diffuse,
        const glm::vec3& specular,
        float shininess)
    {
        auto material = CreateRef<PhongMaterial>();
        material->SetAmbient(ambient);
        material->SetDiffuse(diffuse);
        material->SetSpecular(specular);
        material->SetShininess(shininess);
        return material;
    }
}

#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/MaterialX/MaterialXMaterialReader.h"

#if defined(OLO_WITH_MATERIALX)

#include "OloEngine/Core/Log.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mx = MaterialX;

namespace OloEngine::MaterialXImport
{
    namespace
    {
        // Input-name sets differ by shader model — a single .mtlx may carry any of these,
        // so the mapper branches on node category before reading.
        struct InputNames
        {
            const char* BaseColor;
            const char* Metallic;
            const char* Roughness;
            const char* Emissive;
            const char* EmissiveWeight; // nullptr when the model has no separate weight
        };

        InputNames NamesForCategory(const std::string& category)
        {
            if (category == "UsdPreviewSurface")
                return { "diffuseColor", "metallic", "roughness", "emissiveColor", nullptr };
            if (category == "gltf_pbr")
                return { "base_color", "metallic", "roughness", "emissive", "emissive_strength" };
            // standard_surface (Autodesk) — the common default and closest-match fallback.
            return { "base_color", "metalness", "specular_roughness", "emission_color", "emission" };
        }

        std::optional<glm::vec4> ReadColor4(const mx::NodePtr& node, const char* name)
        {
            if (!name)
                return std::nullopt;
            mx::InputPtr input = node->getInput(name);
            if (!input || !input->hasValue())
                return std::nullopt;
            mx::ValuePtr value = input->getValue();
            if (!value)
                return std::nullopt;
            if (value->isA<mx::Color3>())
            {
                auto c = value->asA<mx::Color3>();
                return glm::vec4(c[0], c[1], c[2], 1.0f);
            }
            if (value->isA<mx::Color4>())
            {
                auto c = value->asA<mx::Color4>();
                return glm::vec4(c[0], c[1], c[2], c[3]);
            }
            if (value->isA<mx::Vector3>())
            {
                auto c = value->asA<mx::Vector3>();
                return glm::vec4(c[0], c[1], c[2], 1.0f);
            }
            return std::nullopt;
        }

        std::optional<f32> ReadFloat(const mx::NodePtr& node, const char* name)
        {
            if (!name)
                return std::nullopt;
            mx::InputPtr input = node->getInput(name);
            if (!input || !input->hasValue())
                return std::nullopt;
            mx::ValuePtr value = input->getValue();
            if (value && value->isA<float>())
                return value->asA<float>();
            return std::nullopt;
        }

        // Log the resolved file path of any texture-driven input (first-slice: paths are not
        // yet uploaded into the material's map slots — that needs a GL context + asset manager).
        void LogTextureInput(const mx::NodePtr& shader, const char* inputName, const char* channel)
        {
            if (!inputName)
                return;
            mx::InputPtr input = shader->getInput(inputName);
            if (!input)
                return;
            mx::NodePtr upstream = input->getConnectedNode();
            if (!upstream)
                return;
            if (upstream->getCategory() == "normalmap")
            {
                if (mx::InputPtr inner = upstream->getInput("in"))
                    upstream = inner->getConnectedNode();
                if (!upstream)
                    return;
            }
            if (mx::InputPtr fileInput = upstream->getInput("file"))
            {
                const std::string resolved = fileInput->getResolvedValueString();
                OLO_CORE_TRACE("MaterialX: {} channel is texture-driven -> '{}' (texture wiring is a follow-up)",
                               channel, resolved);
            }
        }

        void MapShaderToMaterial(const mx::NodePtr& shader, Material& material)
        {
            const std::string category = shader->getCategory();
            const InputNames names = NamesForCategory(category);

            material.SetType(MaterialType::PBR);

            if (auto baseColor = ReadColor4(shader, names.BaseColor))
                material.SetBaseColorFactor(*baseColor);
            if (auto metallic = ReadFloat(shader, names.Metallic))
                material.SetMetallicFactor(std::isfinite(*metallic) ? *metallic : 0.0f);
            if (auto roughness = ReadFloat(shader, names.Roughness))
                material.SetRoughnessFactor(std::isfinite(*roughness) ? *roughness : 1.0f);

            if (auto emissive = ReadColor4(shader, names.Emissive))
            {
                glm::vec4 e = *emissive;
                // standard_surface / gltf_pbr scale the emissive colour by a separate weight.
                if (names.EmissiveWeight)
                {
                    if (auto weight = ReadFloat(shader, names.EmissiveWeight); weight && std::isfinite(*weight))
                        e *= *weight;
                }
                material.SetEmissiveFactor(e);
            }

            LogTextureInput(shader, names.BaseColor, "baseColor");
            LogTextureInput(shader, "normal", "normal");
            LogTextureInput(shader, names.Roughness, "roughness/metallic");
            LogTextureInput(shader, names.Emissive, "emissive");
        }
    } // namespace

    bool ReadMaterialXMaterial(const std::filesystem::path& path, Material& outMaterial, std::string& outError)
    {
        mx::DocumentPtr doc = mx::createDocument();
        try
        {
            mx::readFromXmlFile(doc, mx::FilePath(path.string()));
        }
        catch (const std::exception& e)
        {
            outError = std::string("MaterialX: failed to read '") + path.string() + "': " + e.what();
            return false;
        }

        // Preferred: a surfacematerial node -> its connected surface shader.
        for (const mx::NodePtr& materialNode : doc->getMaterialNodes())
        {
            std::vector<mx::NodePtr> shaders = mx::getShaderNodes(materialNode, mx::SURFACE_SHADER_TYPE_STRING);
            if (!shaders.empty())
            {
                MapShaderToMaterial(shaders.front(), outMaterial);
                outMaterial.SetName(materialNode->getName());
                return true;
            }
        }

        // Fallback: a bare surface-shader node with no surfacematerial wrapper.
        for (const mx::NodePtr& node : doc->getNodes())
        {
            const std::string category = node->getCategory();
            if (category == "standard_surface" || category == "UsdPreviewSurface" ||
                category == "gltf_pbr" || category == "open_pbr_surface")
            {
                MapShaderToMaterial(node, outMaterial);
                outMaterial.SetName(node->getName());
                return true;
            }
        }

        outError = "MaterialX: no recognized surface shader (standard_surface / UsdPreviewSurface / gltf_pbr) in " +
                   path.string();
        return false;
    }
} // namespace OloEngine::MaterialXImport

#endif // OLO_WITH_MATERIALX

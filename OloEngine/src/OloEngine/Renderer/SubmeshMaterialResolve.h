#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"

namespace OloEngine
{
    /**
     * @brief The ONE place that decides which material shades a submesh.
     *
     * Precedence: an explicit override (the entity's MaterialComponent) beats the material
     * the submesh was IMPORTED with, which beats the engine default.
     *
     * This exists because the classic and virtualized submission paths each implemented
     * half of it and drifted: Renderer3D::SubmitVirtualMesh did override -> imported ->
     * default, Model::DrawParallel did imported -> default (no override), and the
     * Scene MeshComponent / SubmeshComponent / skinned-mesh loops did override -> default
     * (no imported), so a multi-material glTF pushed through a MeshComponent shaded every
     * one of its submeshes with the flat engine-default material. Route every submission
     * path through these two functions so the paths cannot diverge again (issue #629).
     *
     * A procedurally-built MeshSource carries no imported materials at all — then
     * GetImportedMaterialForSubmesh() returns null and the default applies cleanly.
     */
    [[nodiscard]] inline const Material& ResolveSubmeshMaterial(const Material* overrideMaterial,
                                                                const Material* importedMaterial,
                                                                const Material& defaultMaterial)
    {
        if (overrideMaterial != nullptr)
        {
            return *overrideMaterial;
        }
        if (importedMaterial != nullptr)
        {
            return *importedMaterial;
        }
        return defaultMaterial;
    }

    /// Same precedence, pulling the imported material out of the MeshSource by submesh index
    /// (which maps through Submesh::m_MaterialIndex). A null meshSource, an out-of-range
    /// index, or a source with no imported materials all fall through to the default.
    [[nodiscard]] inline const Material& ResolveSubmeshMaterial(const Material* overrideMaterial,
                                                                const MeshSource* meshSource,
                                                                u32 submeshIndex,
                                                                const Material& defaultMaterial)
    {
        if (overrideMaterial != nullptr)
        {
            return *overrideMaterial;
        }
        if (meshSource != nullptr)
        {
            // A raw observer into the MeshSource's persistent m_ImportedMaterials — NOT a local
            // owning Ref. Returning *imported through a local Ref<Material> destroyed at return
            // was a returned-reference-to-a-just-released-owner pattern (SonarQube "use of memory
            // after it is freed"); the referent is owned by the MeshSource, which every caller
            // keeps alive across its use of the result, so the observer is the honest shape.
            if (const Material* imported = meshSource->GetImportedMaterialPtrForSubmesh(submeshIndex); imported != nullptr)
            {
                return *imported;
            }
        }
        return defaultMaterial;
    }

    /// Shadow-casting is a property of the RESOLVED material, not of the entity: the shared
    /// shadow-depth shader samples no albedo alpha, so an alpha-masked / blended submesh
    /// would project its full geometry as a solid silhouette. Every submission path asks
    /// this question, so it lives next to the resolve.
    [[nodiscard]] inline bool MaterialCastsShadows(const Material& material)
    {
        return !material.GetFlag(MaterialFlag::DisableShadowCasting) && material.GetAlphaMode() == AlphaMode::Opaque;
    }
} // namespace OloEngine

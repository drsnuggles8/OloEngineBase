#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GltfPhysicalMaterial.h"

#include "OloEngine/Renderer/Material.h"

#include <assimp/material.h>
#include <assimp/scene.h>

#include <atomic>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // Process-wide because the counters answer "did any asset this session
        // carry something we dropped?", which is a property of the run, not of
        // one importer instance. Relaxed ordering: these are diagnostics, and
        // no other state is published through them.
        std::atomic<u64> s_TransmissionMapsIgnored{ 0 };
        std::atomic<u64> s_ThicknessMapsIgnored{ 0 };
        std::atomic<u64> s_TransmissiveDrawsWithoutForwardOverlay{ 0 };
        std::atomic<bool> s_WarnedTransmissiveDrawWithoutForwardOverlay{ false };
    } // namespace

    void ImportGltfPhysicalMaterial(const aiMaterial* mat, Material& material)
    {
        if (!mat)
            return;

        // --- KHR_materials_transmission -----------------------------------
        // Absent on every non-glTF format and on a glTF that doesn't use the
        // extension, which leaves the neutral 0 and makes this import a no-op.
        if (f32 transmission = 0.0f; mat->Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission) == AI_SUCCESS)
        {
            // Every setter sanitizes (isfinite + clamp) — a NaN transmission
            // from a malformed asset must not reach the UBO.
            material.SetTransmissionFactor(transmission);
        }

        // --- KHR_materials_ior --------------------------------------------
        // Assimp maps the extension onto the generic $mat.refracti key -- and
        // that generality is the problem, which is why this is read ONLY for a
        // material that actually transmits.
        //
        // OBJ/MTL writes `Ni` on ordinary opaque materials, routinely as 1.0, and
        // Assimp's OBJ importer supplies a default even when the file does not.
        // Importing that unconditionally would give every OBJ material a
        // "non-default" IOR: they would re-serialize with a PhysicalMaterial
        // block they never had, breaking the byte-identical re-save this feature
        // promises, and an IOR of 1.0 means F0 = 0, so raising transmission on
        // such a material later would leave it with no Fresnel at all.
        //
        // The cost is that a glTF carrying KHR_materials_ior with NO transmission
        // does not round-trip its ior. That is acceptable and documented: the
        // shading path consumes IOR only inside the transmission closure, so on
        // such a material the value has no observable effect either way.
        if (material.IsTransmissive())
        {
            if (f32 ior = kDefaultIOR; mat->Get(AI_MATKEY_REFRACTI, ior) == AI_SUCCESS)
            {
                material.SetIOR(ior);
            }
        }

        // --- KHR_materials_volume -----------------------------------------
        if (f32 thickness = 0.0f; mat->Get(AI_MATKEY_VOLUME_THICKNESS_FACTOR, thickness) == AI_SUCCESS)
        {
            material.SetThicknessFactor(thickness);
        }

        if (f32 attenuationDistance = std::numeric_limits<f32>::infinity();
            mat->Get(AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, attenuationDistance) == AI_SUCCESS)
        {
            // +infinity is legal and meaningful here ("no absorption"), so
            // SetAttenuationDistance deliberately accepts a non-finite value.
            material.SetAttenuationDistance(attenuationDistance);
        }

        if (aiColor3D attenuationColor(1.0f, 1.0f, 1.0f);
            mat->Get(AI_MATKEY_VOLUME_ATTENUATION_COLOR, attenuationColor) == AI_SUCCESS)
        {
            material.SetAttenuationColor(glm::vec3(attenuationColor.r, attenuationColor.g, attenuationColor.b));
        }

        // --- Unsupported in this slice, reported rather than absorbed ------
        //
        // Both extensions put their textures on aiTextureType_TRANSMISSION, told
        // apart by SEMANTIC INDEX: 0 is the transmission map, 1 the volume
        // thickness map. This slice honours the scalar factors only, so a map
        // means the imported material is a simplification of the asset — which
        // the user is told about, once per material, and which a test can count.
        //
        // EACH INDEX IS PROBED DIRECTLY, not inferred from GetTextureCount().
        // That returns how MANY textures carry the semantic, not the highest
        // index in use, so a material with a thickness map and no transmission
        // map has count 1 — and reading that as "index 0 is present" reported a
        // thickness texture as a transmission one and left ThicknessMapsIgnored
        // at zero.
        aiString ignoredTexturePath;
        const bool hasTransmissionMap =
            mat->GetTexture(aiTextureType_TRANSMISSION, 0, &ignoredTexturePath) == AI_SUCCESS;
        const bool hasThicknessMap =
            mat->GetTexture(aiTextureType_TRANSMISSION, 1, &ignoredTexturePath) == AI_SUCCESS;

        if (hasTransmissionMap || hasThicknessMap)
        {
            aiString materialName;
            mat->Get(AI_MATKEY_NAME, materialName);

            if (hasTransmissionMap)
            {
                s_TransmissionMapsIgnored.fetch_add(1, std::memory_order_relaxed);
                OLO_CORE_WARN("ImportGltfPhysicalMaterial: '{}' carries a KHR_materials_transmission texture, which is "
                              "not supported — importing the transmissionFactor {} alone. See "
                              "docs/guides/gltf-material-extensions.md.",
                              materialName.C_Str(), material.GetTransmissionFactor());
            }

            // THE THICKNESS TEXTURE IS NO LONGER REPORTED HERE, and the reason
            // is ORDERING rather than a change of policy. Since issue #1242 the
            // map IS supported -- the importers probe semantic index 1 and call
            // Material::SetThicknessMap -- but they do that AFTER calling this
            // function, so at this point `material` cannot yet know whether the
            // load succeeded. Reporting here would warn about every thickness
            // texture in the project, including the ones that loaded fine.
            //
            // So the report moved to NoteThicknessMapUnloadable(), which the
            // importers call once the outcome is known. `hasThicknessMap` stays
            // computed above because the transmission branch shares the probe.
            (void)hasThicknessMap;
        }
    }

    void NoteThicknessMapUnloadable(std::string_view materialName, f32 thicknessFactor)
    {
        s_ThicknessMapsIgnored.fetch_add(1, std::memory_order_relaxed);
        OLO_CORE_WARN("ImportGltfPhysicalMaterial: '{}' declares a KHR_materials_volume thickness texture that could "
                      "not be loaded — importing the thicknessFactor {} alone, so any thin region shades as uniformly "
                      "thick. See docs/guides/skin-transmission.md.",
                      materialName, thicknessFactor);
    }

    void NoteTransmissiveDrawWithoutForwardOverlay()
    {
        s_TransmissiveDrawsWithoutForwardOverlay.fetch_add(1, std::memory_order_relaxed);

        // Warn ONCE. This runs per draw per frame, so an unconditional warning
        // would bury the log at 60 Hz; the counter carries the magnitude.
        if (!s_WarnedTransmissiveDrawWithoutForwardOverlay.exchange(true, std::memory_order_relaxed))
        {
            OLO_CORE_WARN("Renderer3D: a transmissive material was submitted on the Deferred path with no "
                          "ForwardOverlayPass to reroute it to. The G-Buffer carries no transmission channels, so "
                          "this draw shades OPAQUE. Use Forward/Forward+ for transmissive materials, or enable the "
                          "forward overlay pass. See docs/guides/gltf-material-extensions.md.");
        }
    }

    PhysicalMaterialStats GetPhysicalMaterialStats()
    {
        return PhysicalMaterialStats{
            .TransmissionMapsIgnored = s_TransmissionMapsIgnored.load(std::memory_order_relaxed),
            .ThicknessMapsIgnored = s_ThicknessMapsIgnored.load(std::memory_order_relaxed),
            .TransmissiveDrawsWithoutForwardOverlay =
                s_TransmissiveDrawsWithoutForwardOverlay.load(std::memory_order_relaxed),
        };
    }

    void ResetPhysicalMaterialStats()
    {
        s_TransmissionMapsIgnored.store(0, std::memory_order_relaxed);
        s_ThicknessMapsIgnored.store(0, std::memory_order_relaxed);
        s_TransmissiveDrawsWithoutForwardOverlay.store(0, std::memory_order_relaxed);
        s_WarnedTransmissiveDrawWithoutForwardOverlay.store(false, std::memory_order_relaxed);
    }

} // namespace OloEngine

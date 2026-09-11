#pragma once

#include "OloEngine/Core/Base.h"

struct aiMaterial;

namespace OloEngine
{
    class Material;

    // Import of the three physical glTF material extensions (issue #970):
    // KHR_materials_transmission, KHR_materials_ior and KHR_materials_volume.
    //
    // ONE implementation, called from BOTH glTF import routes —
    // Model::ProcessMaterial and AnimatedModel's material pass. They are
    // separate walks over the same aiScene, and every earlier material feature
    // that was added to one and not the other (alpha mode, cutoff) produced a
    // static mesh and a skinned mesh of the same asset that shaded differently.
    //
    // Assimp exposes all three extensions through documented material keys, so
    // nothing here parses JSON: transmission is $mat.transmission.factor, IOR
    // rides the generic $mat.refracti, and the volume trio lives under
    // $mat.volume.*. A format that sets none of them (OBJ, FBX, legacy glTF)
    // leaves every field at its neutral default and imports exactly as before.
    //
    // IOR IS READ ONLY FOR A MATERIAL THAT ALREADY TRANSMITS. Assimp routes the
    // extension onto the GENERIC `$mat.refracti` key, which OBJ/MTL also writes
    // on ordinary opaque materials — routinely as 1.0, and Assimp's OBJ importer
    // supplies a default even when the file does not. Importing it
    // unconditionally would give every OBJ material a "non-default" IOR, so they
    // would re-serialize with a PhysicalMaterial block they never had, and an
    // IOR of 1.0 means F0 = 0. The cost is that a glTF with KHR_materials_ior
    // and no transmission does not round-trip its ior; since the shading path
    // consumes IOR only inside the transmission closure, that value has no
    // observable effect on such a material either way.
    void ImportGltfPhysicalMaterial(const aiMaterial* mat, Material& material);

    // Everything about physical materials that was met and could NOT be
    // honoured, on either side of the pipeline.
    //
    // CLAUDE.md forbids a silent fallback, and each counter below is a place
    // where the honest answer is "this asset asked for something this slice
    // does not do". Each is warned ONCE (so a per-draw case cannot spam the
    // log) and counted ALWAYS, so a test asserts the count instead of scraping
    // output. See docs/guides/gltf-material-extensions.md.
    struct PhysicalMaterialStats
    {
        // Import: the transmission / thickness TEXTURES. This slice supports
        // the scalar factors only, so a mapped asset imports at its factor.
        u64 TransmissionMapsIgnored = 0;
        u64 ThicknessMapsIgnored = 0;

        // Render: a transmissive material met on the Deferred path with no
        // ForwardOverlayPass to reroute it to. The G-Buffer has no transmission
        // channels, so such a draw shades OPAQUE — visibly wrong, and counted
        // here rather than left to be discovered on screen.
        u64 TransmissiveDrawsWithoutForwardOverlay = 0;
    };

    [[nodiscard]] PhysicalMaterialStats GetPhysicalMaterialStats();

    // Called from the draw-submission routing when a transmissive material has
    // nowhere correct to go on the Deferred path.
    void NoteTransmissiveDrawWithoutForwardOverlay();

    // Test-only: zero the counters so a case can assert on its own work.
    void ResetPhysicalMaterialStats();

} // namespace OloEngine

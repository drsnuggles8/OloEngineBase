#pragma once

// The MaterialX (.mtlx) reader only exists when the OLO_WITH_MATERIALX CMake option compiled
// the MaterialX dependency in. When off, this header is empty and no MaterialX code is linked.
#if defined(OLO_WITH_MATERIALX)

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Material.h"

#include <filesystem>
#include <string>

namespace OloEngine::MaterialXImport
{
    // Reads a standalone MaterialX document (.mtlx) and maps its surface shader
    // (standard_surface / UsdPreviewSurface / gltf_pbr) onto the engine's PBR Material
    // (issue #655). Maps the scalar/colour FACTORS — base color, metallic, roughness,
    // emissive — which fully define an untextured material and are GL-free (so this is unit
    // testable headless). Texture inputs are resolved to their file paths and logged; wiring
    // those paths through the asset manager into the material's map slots is a follow-up
    // (documented in the PR) because texture upload needs a GL context.
    //
    // Returns true and fills `outMaterial` on success; false with `outError` set otherwise.
    // Reads only explicitly-authored inputs (no MaterialX stdlib required at runtime) and
    // leaves any unauthored parameter at the Material's own constructor default.
    bool ReadMaterialXMaterial(const std::filesystem::path& path, Material& outMaterial, std::string& outError);
} // namespace OloEngine::MaterialXImport

#endif // OLO_WITH_MATERIALX

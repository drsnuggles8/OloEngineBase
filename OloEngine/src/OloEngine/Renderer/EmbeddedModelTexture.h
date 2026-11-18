#pragma once

// A texture a model file carries inside itself (glTF binary chunks and data URIs, FBX
// embedded media), which Assimp reports as "*N" or by the texture's own file name.
//
// One loader for both importers. Model learned to cook these long ago; AnimatedModel
// took "*0" for a file name beside the model and found nothing, so a skinned or
// morph-target GLB that embedded its maps came through with none. That stayed out of
// sight while only rigged files took the animated route, and became a regression the
// moment a file with morph targets and no skeleton was routed there too.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>

struct aiTexture;

namespace OloEngine
{
    class Texture2D;

    // Cook the embedded bitmap into the asset cache and load it from there, so that the
    // material codec can persist it by path; fall back to a GPU-only decode when the cook
    // cannot be written. Null when the bitmap cannot be decoded at all.
    //
    // `modelSourcePath` is the model file, `assimpTextureRef` the reference the material
    // names it by: together they are the cooked file's identity.
    [[nodiscard]] Ref<Texture2D> LoadEmbeddedModelTexture(const aiTexture* embedded, const std::string& modelSourcePath,
                                                          const std::string& assimpTextureRef, bool srgb);
} // namespace OloEngine

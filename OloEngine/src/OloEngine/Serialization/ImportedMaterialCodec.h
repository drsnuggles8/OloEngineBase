#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Renderer/Material.h"

#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vector>

namespace OloEngine
{
    class Material;

    // ============================================================================
    // Imported-material codec (issue #629)
    //
    // MeshSource::m_ImportedMaterials holds the Ref<Material> objects a mesh was
    // imported with (one per UNIQUE source material; Submesh::m_MaterialIndex
    // indexes it). Neither container that persists a MeshSource — the .omesh dev
    // cache nor the shipped asset pack — used to carry them, so a PACKED runtime,
    // which has no source file and no Assimp importer, resolved every submesh to
    // the flat engine-default material: ship the game, lose every texture.
    //
    // This codec turns that vector into ONE self-describing blob that both
    // containers embed verbatim (.omesh: an appended section; asset pack: an
    // appended, version-gated trailing field). One wire format, two carriers.
    //
    // Textures are referenced by ASSET HANDLE, not by pixels and not (primarily)
    // by path:
    //   * the packed runtime has no loose files, so a path is unusable there —
    //     but the textures ARE separate pack entries, addressed by handle;
    //   * a handle is shared, so N materials referencing one texture cost N*8
    //     bytes, not N copies of the image.
    // The source path + sRGB intent ride along as an EDITOR-ONLY fallback: a
    // texture that Model loaded straight off disk (Texture2D::Create) has no
    // registered handle until the file is imported as an asset, and the .omesh
    // cache must still round-trip it. Realize() prefers the handle and falls
    // back to the path only when the file is actually there.
    //
    // EMBEDDED TEXTURES (was a known gap; now closed upstream of this codec).
    // A texture embedded in the model file (glTF/FBX base64 data URI or binary chunk) has
    // neither an asset handle nor an on-disk path of its own, so it used to survive NEITHER
    // container: Describe() produced an empty TextureRef and the material came back with its
    // factors but no maps — a shipped build silently lost the textures of every model that
    // embeds them, which is most GLB files.
    //
    // The fix is in the IMPORTER, not here: Model::LoadMaterialTextures now COOKS an embedded
    // bitmap into a real .png asset under <AssetDir>/cache/embedded/ and ImportAsset()s it, so
    // it gains a stable, deterministic path AND a registry handle like any other texture — after
    // which the handle-plus-path scheme below already works, unchanged. The cooked name is
    // derived from FNV-1a-64(model path | Assimp texture ref) plus the colour-space intent, so
    // it is stable across re-imports (the pack does not churn and scene references do not rot),
    // and the intent is spelled into the filename because the packed-runtime loader and the pack
    // cook both decide sRGB from the FILENAME.
    // Pinned by ImportedMaterialPackTest.EmbeddedTextureSurvivesThe{OmeshCache,AssetPack}.
    //
    // STILL OPEN: AnimatedModel::LoadMaterialTextures (the rigged/skinned importer) never had
    // embedded-texture support at all — it does not take the aiScene and never calls
    // GetEmbeddedTexture, so a "*0" reference falls through to the on-disk loader and fails.
    // A rigged GLB therefore loses its embedded textures at IMPORT time, before serialization is
    // even reached. That is a separate importer bug, not a codec one.
    // ============================================================================
    namespace ImportedMaterialCodec
    {
        constexpr u32 MagicNumber = 0x54414D4F; // "OMAT" little-endian
        constexpr u32 CurrentVersion = 1;

        // ── Safety caps (defence against a corrupt/hostile blob) ──
        constexpr u32 MaxMaterialCount = 10'000;
        constexpr u32 MaxStringLength = 4'096;
        constexpr u64 MaxBlobSize = 64'000'000; // 64 MB — descriptors only, no pixels

        // One texture slot of a material.
        struct TextureRef
        {
            AssetHandle Handle = 0; // 0 = not a registered asset
            std::string Path;       // editor-only fallback (may be empty)
            bool SRGB = false;      // colour-space intent the texture was loaded with

            [[nodiscard]] bool IsEmpty() const
            {
                return static_cast<u64>(Handle) == 0 && Path.empty();
            }
        };

        // A serializable description of one imported Material. Deliberately a flat
        // POD-ish record rather than a MaterialAsset handle: MaterialAsset's YAML
        // carries neither AlphaMode nor AlphaCutoff nor NormalScale nor
        // OcclusionStrength, and every alpha-masked material (Sponza's foliage,
        // every glTF MASK material) would silently render opaque through it.
        struct MaterialDesc
        {
            // A null slot in the source vector stays a null slot: Submesh::m_MaterialIndex
            // addresses the table POSITIONALLY, so entries can never be compacted away.
            bool Present = true;
            std::string Name;
            i32 Type = static_cast<i32>(MaterialType::PBR);
            u32 Flags = 0; // MaterialFlag bitfield
            i32 AlphaMode = static_cast<i32>(AlphaMode::Opaque);
            f32 AlphaCutoff = 0.5f;
            glm::vec4 BaseColorFactor{ 1.0f };
            glm::vec4 EmissiveFactor{ 0.0f };
            f32 MetallicFactor = 0.0f;
            f32 RoughnessFactor = 1.0f;
            f32 NormalScale = 1.0f;
            f32 OcclusionStrength = 1.0f;
            bool EnableIBL = false;

            TextureRef Albedo;
            TextureRef MetallicRoughness;
            TextureRef Normal;
            TextureRef AO;
            TextureRef Emissive;
        };

        // Material -> descriptor. Resolves each texture's asset handle: the
        // texture's own handle when it has one, else the registry handle of its
        // source path (editor only). Never touches the GPU.
        [[nodiscard]] std::vector<MaterialDesc> Describe(const std::vector<Ref<Material>>& materials);

        // Descriptor -> Material. Resolves each texture through the AssetManager by
        // handle, falling back to loading the source path off disk when the handle
        // is unknown but the file exists. A slot that resolves to nothing is simply
        // left unset (the consumer's own default applies), never a hard failure.
        [[nodiscard]] std::vector<Ref<Material>> Realize(const std::vector<MaterialDesc>& descs);

        // Wire format (self-describing; own magic + version).
        [[nodiscard]] std::vector<u8> Encode(const std::vector<MaterialDesc>& descs);
        [[nodiscard]] bool Decode(std::span<const u8> blob, std::vector<MaterialDesc>& outDescs);

        // Convenience: the two halves both carriers actually call.
        [[nodiscard]] std::vector<u8> EncodeMaterials(const std::vector<Ref<Material>>& materials);
        [[nodiscard]] bool DecodeMaterials(std::span<const u8> blob, std::vector<Ref<Material>>& outMaterials);
    } // namespace ImportedMaterialCodec
} // namespace OloEngine

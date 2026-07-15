#include "OloEnginePCH.h"

#include "OloEngine/Serialization/ImportedMaterialCodec.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>

namespace OloEngine::ImportedMaterialCodec
{
    namespace
    {
        // ── Byte-level writers/readers over a plain std::vector<u8> ──────────
        // Little-endian, matching every other binary format in the engine
        // (the .omesh / .olopack containers assume LE throughout).

        template<typename T>
        void WritePod(std::vector<u8>& out, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }

        void WriteString(std::vector<u8>& out, const std::string& value)
        {
            auto const length = static_cast<u32>(value.size());
            WritePod(out, length);
            out.insert(out.end(), value.begin(), value.end());
        }

        struct Cursor
        {
            std::span<const u8> Data;
            sizet Offset = 0;

            [[nodiscard]] bool Remaining(sizet bytes) const
            {
                return Offset + bytes <= Data.size();
            }
        };

        template<typename T>
        [[nodiscard]] bool ReadPod(Cursor& cursor, T& outValue)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (!cursor.Remaining(sizeof(T)))
            {
                return false;
            }
            std::memcpy(&outValue, cursor.Data.data() + cursor.Offset, sizeof(T));
            cursor.Offset += sizeof(T);
            return true;
        }

        [[nodiscard]] bool ReadString(Cursor& cursor, std::string& outValue)
        {
            u32 length = 0;
            if (!ReadPod(cursor, length))
            {
                return false;
            }
            if (length > MaxStringLength || !cursor.Remaining(length))
            {
                return false;
            }
            outValue.assign(reinterpret_cast<const char*>(cursor.Data.data() + cursor.Offset), length);
            cursor.Offset += length;
            return true;
        }

        void WriteTextureRef(std::vector<u8>& out, const TextureRef& ref)
        {
            WritePod(out, static_cast<u64>(ref.Handle));
            WritePod(out, static_cast<u8>(ref.SRGB ? 1 : 0));
            WriteString(out, ref.Path);
        }

        [[nodiscard]] bool ReadTextureRef(Cursor& cursor, TextureRef& outRef)
        {
            u64 handle = 0;
            u8 srgb = 0;
            if (!ReadPod(cursor, handle) || !ReadPod(cursor, srgb) || !ReadString(cursor, outRef.Path))
            {
                return false;
            }
            outRef.Handle = AssetHandle(handle);
            outRef.SRGB = srgb != 0;
            return true;
        }

        [[nodiscard]] f32 SanitizeFloat(f32 value, f32 fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        // Enum fields ride the wire as raw ints. A corrupt/hostile blob can carry a value
        // no enumerator matches, which then flows into shader-selection / alpha-queue
        // switches that assume a valid case — a silently mis-rendered material (a MASK/BLEND
        // surface drawn opaque, or the wrong shader picked). Clamp to the known range per
        // field, exactly as the floats above fall back to a default. `[lo, hi]` inclusive.
        [[nodiscard]] i32 SanitizeEnum(i32 value, i32 lo, i32 hi, i32 fallback)
        {
            return (value >= lo && value <= hi) ? value : fallback;
        }

        [[nodiscard]] glm::vec4 SanitizeVec4(const glm::vec4& value, const glm::vec4& fallback)
        {
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) || !std::isfinite(value.w))
            {
                return fallback;
            }
            return value;
        }

        // The editor asset manager, or nullptr when we are not running against one
        // (packed runtime, or a test with no project). Never asserts.
        [[nodiscard]] EditorAssetManager* TryGetEditorAssetManager()
        {
            if (!Project::GetActive())
            {
                return nullptr;
            }
            Ref<AssetManagerBase> manager = Project::GetAssetManager();
            if (!manager)
            {
                return nullptr;
            }
            return dynamic_cast<EditorAssetManager*>(manager.Raw());
        }

        // A texture imported straight off disk (Model's Texture2D::Create path) has no
        // asset handle of its own. In the editor the same file is virtually always a
        // registered Texture asset — and it is that entry, not the loose file, that the
        // asset pack ships. Resolve it so the packed runtime can find the texture again.
        [[nodiscard]] AssetHandle ResolveTextureHandle(const Ref<Texture2D>& texture)
        {
            if (!texture)
            {
                return AssetHandle(0);
            }
            if (static_cast<u64>(texture->GetHandle()) != 0)
            {
                return texture->GetHandle();
            }

            const std::string& path = texture->GetPath();
            if (path.empty())
            {
                return AssetHandle(0);
            }

            EditorAssetManager* editor = TryGetEditorAssetManager();
            if (!editor)
            {
                return AssetHandle(0);
            }
            // GetAssetHandleFromFilePath normalises for us: an absolute path (which is what
            // the asset-manager-driven import produces — Project dir / metadata path) is made
            // project-relative, a relative one is looked up as-is. Doing the GetRelativePath
            // step here would resolve a relative path against the CWD instead, which is not
            // the project root.
            return editor->GetAssetHandleFromFilePath(path);
        }

        [[nodiscard]] TextureRef DescribeTexture(const Ref<Texture2D>& texture)
        {
            TextureRef ref;
            if (!texture)
            {
                return ref;
            }
            ref.Handle = ResolveTextureHandle(texture);
            ref.Path = texture->GetPath();
            ref.SRGB = texture->GetSpecification().SRGB;
            return ref;
        }

        [[nodiscard]] Ref<Texture2D> RealizeTexture(const TextureRef& ref)
        {
            // SOURCE FILE FIRST, asset handle only as the fallback. This order is
            // load-bearing, not a preference:
            //
            // The importer knows a texture's colour space from the material SLOT it fills
            // (Model::LoadMaterialTextures: DIFFUSE / BASE_COLOR / EMISSIVE -> sRGB, normal /
            // metal-rough / AO -> linear) and we record exactly that in ref.SRGB. The asset
            // manager does NOT: TextureSerializer::TryLoadData guesses the colour space from
            // the FILE NAME (IsLikelyColorTextureByName). Sponza's textures are hash-named
            // ("466164707995436622.jpg"), so that guess says "not a colour texture" and every
            // albedo map would come back LINEAR — and any file the asset path fails to load at
            // all comes back null, which makes an alpha-masked material discard the whole
            // submesh (it vanishes) and an opaque one shade black.
            //
            // Resolving from the recorded path + recorded intent reproduces the cold import
            // byte for byte, so a warm load is identical to a fresh one no matter WHICH
            // importer wrote the cache. Before this, a .omesh written by the MeshSource
            // importer (absolute path -> registry handle resolves -> handles stored) rendered
            // differently from one written by the ModelComponent path (relative path -> no
            // handle -> path stored) — the same asset, two different results, decided by
            // whichever scene was opened first (issue #629).
            if (!ref.Path.empty())
            {
                std::error_code ec;
                if (std::filesystem::exists(ref.Path, ec) && !ec)
                {
                    if (Ref<Texture2D> texture = Texture2D::Create(ref.Path, ref.SRGB); texture && texture->IsLoaded())
                    {
                        return texture;
                    }
                }
            }

            // Packed runtime: there are no loose files, and the texture ships in the pack
            // under this handle. (The pack records each texture's own sRGB flag, so the
            // colour space comes from the cooked record rather than from us here.)
            if (static_cast<u64>(ref.Handle) != 0 && Project::GetActive() && Project::GetAssetManager())
            {
                if (Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(ref.Handle))
                {
                    if (texture->GetSpecification().SRGB != ref.SRGB)
                    {
                        // Not fatal — the texture is still the right pixels — but it will be
                        // gamma-wrong. Say so loudly: the packed texture's colour space is
                        // decided by TextureSerializer's filename heuristic, which cannot see
                        // which material slot the texture fills.
                        OLO_CORE_WARN("ImportedMaterialCodec: packed texture '{}' has SRGB={} but the material slot "
                                      "it fills wants SRGB={} — it will be shaded in the wrong colour space",
                                      ref.Path, texture->GetSpecification().SRGB, ref.SRGB);
                    }
                    return texture;
                }
            }

            if (!ref.IsEmpty())
            {
                OLO_CORE_WARN("ImportedMaterialCodec: could not resolve texture (handle={}, path='{}') — "
                              "the material will render without it",
                              static_cast<u64>(ref.Handle), ref.Path);
            }
            return nullptr;
        }
    } // namespace

    std::vector<MaterialDesc> Describe(const std::vector<Ref<Material>>& materials)
    {
        std::vector<MaterialDesc> descs;
        descs.reserve(materials.size());

        for (const auto& material : materials)
        {
            MaterialDesc desc;
            if (!material)
            {
                // Keep the slot: Submesh::m_MaterialIndex indexes this array by
                // position, so a null entry must stay a (null) entry.
                desc.Present = false;
                descs.push_back(std::move(desc));
                continue;
            }

            desc.Present = true;
            desc.Name = material->GetName();
            desc.Type = static_cast<i32>(material->GetType());
            desc.Flags = material->GetFlags();
            desc.AlphaMode = static_cast<i32>(material->GetAlphaMode());
            desc.AlphaCutoff = material->GetAlphaCutoff();
            desc.BaseColorFactor = material->GetBaseColorFactor();
            desc.EmissiveFactor = material->GetEmissiveFactor();
            desc.MetallicFactor = material->GetMetallicFactor();
            desc.RoughnessFactor = material->GetRoughnessFactor();
            desc.NormalScale = material->GetNormalScale();
            desc.OcclusionStrength = material->GetOcclusionStrength();
            desc.EnableIBL = material->IsIBLEnabled();

            desc.Albedo = DescribeTexture(material->GetAlbedoMap());
            desc.MetallicRoughness = DescribeTexture(material->GetMetallicRoughnessMap());
            desc.Normal = DescribeTexture(material->GetNormalMap());
            desc.AO = DescribeTexture(material->GetAOMap());
            desc.Emissive = DescribeTexture(material->GetEmissiveMap());

            descs.push_back(std::move(desc));
        }

        return descs;
    }

    std::vector<Ref<Material>> Realize(const std::vector<MaterialDesc>& descs)
    {
        std::vector<Ref<Material>> materials;
        materials.reserve(descs.size());

        for (const auto& desc : descs)
        {
            if (!desc.Present)
            {
                materials.push_back(nullptr);
                continue;
            }

            Ref<Material> material = Material::CreatePBR(
                desc.Name,
                glm::vec3(desc.BaseColorFactor),
                desc.MetallicFactor,
                desc.RoughnessFactor);
            if (!material)
            {
                materials.push_back(nullptr);
                continue;
            }

            material->SetType(static_cast<MaterialType>(desc.Type));
            material->SetFlags(desc.Flags);
            material->SetAlphaMode(static_cast<AlphaMode>(desc.AlphaMode));
            material->SetAlphaCutoff(desc.AlphaCutoff);
            material->SetBaseColorFactor(desc.BaseColorFactor);
            material->SetEmissiveFactor(desc.EmissiveFactor);
            material->SetMetallicFactor(desc.MetallicFactor);
            material->SetRoughnessFactor(desc.RoughnessFactor);
            material->SetNormalScale(desc.NormalScale);
            material->SetOcclusionStrength(desc.OcclusionStrength);
            material->SetEnableIBL(desc.EnableIBL);

            // Keep the uniform block in step with the typed factors — CreatePBR seeds
            // it from its arguments, and the renderer reads the uniforms, not just the
            // members (see Renderer3D's material upload).
            material->Set("u_MaterialUniforms.AlbedoColor", glm::vec3(desc.BaseColorFactor));
            material->Set("u_MaterialUniforms.Metalness", desc.MetallicFactor);
            material->Set("u_MaterialUniforms.Roughness", desc.RoughnessFactor);

            if (Ref<Texture2D> albedo = RealizeTexture(desc.Albedo))
            {
                material->SetAlbedoMap(albedo);
            }
            if (Ref<Texture2D> mr = RealizeTexture(desc.MetallicRoughness))
            {
                material->SetMetallicRoughnessMap(mr);
            }
            if (Ref<Texture2D> normal = RealizeTexture(desc.Normal))
            {
                material->SetNormalMap(normal);
            }
            if (Ref<Texture2D> ao = RealizeTexture(desc.AO))
            {
                material->SetAOMap(ao);
            }
            if (Ref<Texture2D> emissive = RealizeTexture(desc.Emissive))
            {
                material->SetEmissiveMap(emissive);
            }

            materials.push_back(material);
        }

        return materials;
    }

    std::vector<u8> Encode(const std::vector<MaterialDesc>& descs)
    {
        std::vector<u8> blob;
        if (descs.empty())
        {
            return blob;
        }
        if (descs.size() > MaxMaterialCount)
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Encode: material count ({}) exceeds cap ({}); dropping the table",
                           descs.size(), MaxMaterialCount);
            return blob;
        }

        WritePod(blob, MagicNumber);
        WritePod(blob, CurrentVersion);
        WritePod(blob, static_cast<u32>(descs.size()));

        for (const auto& desc : descs)
        {
            // Present flag: a null material must survive as a null material, since
            // Submesh::m_MaterialIndex addresses this table positionally.
            WritePod(blob, static_cast<u8>(desc.Present ? 1 : 0));
            if (!desc.Present)
            {
                continue;
            }

            WriteString(blob, desc.Name);
            WritePod(blob, desc.Type);
            WritePod(blob, desc.Flags);
            WritePod(blob, desc.AlphaMode);
            WritePod(blob, desc.AlphaCutoff);
            WritePod(blob, desc.BaseColorFactor);
            WritePod(blob, desc.EmissiveFactor);
            WritePod(blob, desc.MetallicFactor);
            WritePod(blob, desc.RoughnessFactor);
            WritePod(blob, desc.NormalScale);
            WritePod(blob, desc.OcclusionStrength);
            WritePod(blob, static_cast<u8>(desc.EnableIBL ? 1 : 0));

            WriteTextureRef(blob, desc.Albedo);
            WriteTextureRef(blob, desc.MetallicRoughness);
            WriteTextureRef(blob, desc.Normal);
            WriteTextureRef(blob, desc.AO);
            WriteTextureRef(blob, desc.Emissive);
        }

        return blob;
    }

    bool Decode(std::span<const u8> blob, std::vector<MaterialDesc>& outDescs)
    {
        outDescs.clear();
        if (blob.empty())
        {
            return true; // an absent table is not an error — the mesh simply has no materials
        }
        if (blob.size() > MaxBlobSize)
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Decode: blob size ({}) exceeds cap ({})", blob.size(), MaxBlobSize);
            return false;
        }

        Cursor cursor{ blob, 0 };

        u32 magic = 0;
        u32 version = 0;
        u32 count = 0;
        if (!ReadPod(cursor, magic) || !ReadPod(cursor, version) || !ReadPod(cursor, count))
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Decode: truncated header");
            return false;
        }
        if (magic != MagicNumber)
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Decode: bad magic (got {:#x})", magic);
            return false;
        }
        if (version == 0 || version > CurrentVersion)
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Decode: unsupported blob version {} (this build reads 1..{})",
                           version, CurrentVersion);
            return false;
        }
        if (count > MaxMaterialCount)
        {
            OLO_CORE_ERROR("ImportedMaterialCodec::Decode: material count ({}) exceeds cap ({})", count, MaxMaterialCount);
            return false;
        }

        outDescs.reserve(count);
        for (u32 i = 0; i < count; ++i)
        {
            MaterialDesc desc;

            u8 present = 0;
            if (!ReadPod(cursor, present))
            {
                OLO_CORE_ERROR("ImportedMaterialCodec::Decode: truncated at material {}", i);
                return false;
            }
            if (present == 0)
            {
                desc.Present = false;
                outDescs.push_back(std::move(desc));
                continue;
            }

            u8 enableIBL = 0;
            if (!ReadString(cursor, desc.Name) ||
                !ReadPod(cursor, desc.Type) ||
                !ReadPod(cursor, desc.Flags) ||
                !ReadPod(cursor, desc.AlphaMode) ||
                !ReadPod(cursor, desc.AlphaCutoff) ||
                !ReadPod(cursor, desc.BaseColorFactor) ||
                !ReadPod(cursor, desc.EmissiveFactor) ||
                !ReadPod(cursor, desc.MetallicFactor) ||
                !ReadPod(cursor, desc.RoughnessFactor) ||
                !ReadPod(cursor, desc.NormalScale) ||
                !ReadPod(cursor, desc.OcclusionStrength) ||
                !ReadPod(cursor, enableIBL) ||
                !ReadTextureRef(cursor, desc.Albedo) ||
                !ReadTextureRef(cursor, desc.MetallicRoughness) ||
                !ReadTextureRef(cursor, desc.Normal) ||
                !ReadTextureRef(cursor, desc.AO) ||
                !ReadTextureRef(cursor, desc.Emissive))
            {
                OLO_CORE_ERROR("ImportedMaterialCodec::Decode: truncated/invalid material {}", i);
                return false;
            }
            desc.EnableIBL = enableIBL != 0;

            // Enum fields land in a switch that assumes a valid case — validate them the
            // same way the floats below are sanitized. Ranges track the enum definitions in
            // Material.h (MaterialType: Legacy..PBR, AlphaMode: Opaque..Blend).
            desc.Type = SanitizeEnum(desc.Type, static_cast<i32>(MaterialType::Legacy),
                                     static_cast<i32>(MaterialType::PBR), static_cast<i32>(MaterialType::PBR));
            desc.AlphaMode = SanitizeEnum(desc.AlphaMode, static_cast<i32>(AlphaMode::Opaque),
                                          static_cast<i32>(AlphaMode::Blend), static_cast<i32>(AlphaMode::Opaque));

            // Every float lands in a shader UBO — a NaN/Inf from a corrupt blob would
            // poison the frame, so fall back to the engine default per field.
            desc.AlphaCutoff = SanitizeFloat(desc.AlphaCutoff, 0.5f);
            desc.BaseColorFactor = SanitizeVec4(desc.BaseColorFactor, glm::vec4(1.0f));
            desc.EmissiveFactor = SanitizeVec4(desc.EmissiveFactor, glm::vec4(0.0f));
            desc.MetallicFactor = SanitizeFloat(desc.MetallicFactor, 0.0f);
            desc.RoughnessFactor = SanitizeFloat(desc.RoughnessFactor, 1.0f);
            desc.NormalScale = SanitizeFloat(desc.NormalScale, 1.0f);
            desc.OcclusionStrength = SanitizeFloat(desc.OcclusionStrength, 1.0f);

            outDescs.push_back(std::move(desc));
        }

        return true;
    }

    std::vector<u8> EncodeMaterials(const std::vector<Ref<Material>>& materials)
    {
        return Encode(Describe(materials));
    }

    bool DecodeMaterials(std::span<const u8> blob, std::vector<Ref<Material>>& outMaterials)
    {
        outMaterials.clear();

        std::vector<MaterialDesc> descs;
        if (!Decode(blob, descs))
        {
            return false;
        }

        outMaterials = Realize(descs);
        return true;
    }
} // namespace OloEngine::ImportedMaterialCodec

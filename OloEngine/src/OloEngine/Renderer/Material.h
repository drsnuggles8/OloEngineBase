#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace OloEngine
{

    enum class MaterialFlag : u32
    {
        None = 0,
        DepthTest = 1u << 0,
        Blend = 1u << 1,
        TwoSided = 1u << 2,
        DisableShadowCasting = 1u << 3
    };

    enum class MaterialType
    {
        Legacy = 0, // Legacy Phong-style material
        PBR = 1     // Physically Based Rendering material
    };

    // PBRModel (the versioned closure selector, issue #975) lives in
    // Renderer/PBRModel.h — included above — so the GL-free reference path
    // tracer can name it without pulling in Material's renderer dependencies.

    // Matches glTF 2.0 spec alphaMode. Encoded as i32 in shader UBOs.
    enum class AlphaMode : i32
    {
        Opaque = 0, // No alpha test, no blending (default)
        Mask = 1,   // Per-pixel discard when sampled alpha < alphaCutoff
        Blend = 2   // Alpha blending (also requires MaterialFlag::Blend)
    };

    // Physical-material constants (issue #970), shared by Material, the glTF
    // importer, the serializers and the tests so none of them re-spells a
    // magic number.
    //
    // kDefaultIOR is 1.5 because that is BOTH the glTF default and the exact
    // value that reproduces the dielectric F0 the PBR shaders already
    // hardcode: ((1.5 - 1) / (1.5 + 1))^2 == 0.04. That equality is what makes
    // an untouched material bit-identical, and MaterialTransmissionDefaultsTest
    // asserts it rather than trusting the comment.
    inline constexpr f32 kDefaultIOR = 1.5f;
    // Upper bound so a garbage asset cannot push the Fresnel term somewhere
    // useless. Diamond is 2.42; 5 leaves headroom for stylized content.
    inline constexpr f32 kMaxIOR = 5.0f;
    // Positive floor for an attenuation-colour channel, so -log(channel) stays
    // finite. See Material::SetAttenuationColor.
    inline constexpr f32 kMinAttenuationChannel = 1.0e-4f;
    // Positive floor for a FINITE attenuation distance. Rejecting only NaN and
    // non-positive values is not enough: -log(kMinAttenuationChannel) is about
    // 9.2, so a denormal distance near 1e-38 overflows the division to +inf, and
    // an infinite sigma gives exp(-inf * 0) == NaN at thickness 0 -- a NaN in the
    // material UBO, from a value every other check accepts. 1e-4 is far below any
    // meaningful authored distance (it absorbs to black within a tenth of a
    // millimetre) and keeps the derivation comfortably finite.
    inline constexpr f32 kMinAttenuationDistance = 1.0e-4f;

    // @brief Material class for handling PBR and legacy material properties
    //
    // This class uses a consistent encapsulated design with getter/setter methods.
    // All material properties are accessed through typed methods that handle
    // both the uniform system and direct property access efficiently.
    //
    // Features:
    // - Unified interface for both PBR and legacy materials
    // - Automatic uniform management for shader binding
    // - Type-safe property access with validation
    // - Efficient texture and parameter caching
    // - Asset dependency tracking integration
    class Material : public RendererResource
    {
      public:
        // Default constructor for struct-like usage
        Material();

        // Copy constructor for value semantics (Model.cpp uses Material as value type)
        Material(const Material& other);

        // Assignment operator for value semantics
        Material& operator=(const Material& other);

        // No move operations, deliberately. The RendererResource base has a deleted move
        // constructor, so `Material(Material&&) = default;` was defined as deleted and the
        // "efficient transfers" the old comment promised never existed: a defaulted-as-deleted
        // move is IGNORED by overload resolution, so every std::move(Material) already bound
        // to the copy constructor above. Declaring them `= delete` instead would change that
        // -- an explicit delete does participate, turning those calls into hard errors.
        // Leaving them undeclared keeps the copy fallback and drops the misleading
        // -Wdefaulted-function-deleted. Making Material movable means making
        // RendererResource movable first.

        static Ref<Material> Create(const Ref<OloEngine::Shader>& shader, const std::string& name = "");
        static Ref<Material> Copy(const Ref<Material>& other, const std::string& name = "");

        // Static factory method for PBR materials - returns Ref<Material> for consistency
        static Ref<Material> CreatePBR(const std::string& name, const glm::vec3& baseColor, f32 metallic = 0.0f, f32 roughness = 0.5f);
        // Static factory for snow PBR material (white, high roughness, non-metallic)
        static Ref<Material> CreateSnow(const std::string& name = "Snow");

        virtual ~Material() = default;

        virtual void Invalidate() {}
        virtual void OnShaderReloaded() {}

        // Material property accessors
        void SetName(const std::string& name)
        {
            m_Name = name;
        }
        const std::string& GetName() const
        {
            return m_Name;
        }

        void SetType(MaterialType type)
        {
            m_MaterialType = type;
        }
        MaterialType GetType() const
        {
            return m_MaterialType;
        }

        // The versioned PBR closure this material shades with (issue #975).
        // Legacy by default; ClosureV2 is an explicit opt-in. See PBRModel.h.
        void SetPBRModel(PBRModel model)
        {
            m_PBRModel = model;
        }
        PBRModel GetPBRModel() const
        {
            return m_PBRModel;
        }

        void SetShader(const Ref<Shader>& shader)
        {
            m_Shader = shader;
        }
        const Ref<OloEngine::Shader>& GetShader() const
        {
            return m_Shader;
        }

        virtual void Set(const std::string& name, f32 value);
        virtual void Set(const std::string& name, i32 value);
        virtual void Set(const std::string& name, u32 value);
        virtual void Set(const std::string& name, bool value);
        virtual void Set(const std::string& name, const glm::vec2& value);
        virtual void Set(const std::string& name, const glm::vec3& value);
        virtual void Set(const std::string& name, const glm::vec4& value);
        virtual void Set(const std::string& name, const glm::ivec2& value);
        virtual void Set(const std::string& name, const glm::ivec3& value);
        virtual void Set(const std::string& name, const glm::ivec4& value);

        virtual void Set(const std::string& name, const glm::mat3& value);
        virtual void Set(const std::string& name, const glm::mat4& value);

        virtual void Set(const std::string& name, const Ref<Texture2D>& texture);
        virtual void Set(const std::string& name, const Ref<Texture2D>& texture, u32 arrayIndex);
        virtual void Set(const std::string& name, const Ref<TextureCubemap>& texture);

        virtual f32 GetFloat(const std::string& name) const;
        virtual i32 GetInt(const std::string& name) const;
        virtual u32 GetUInt(const std::string& name) const;
        virtual bool GetBool(const std::string& name) const;
        virtual const glm::vec2& GetVector2(const std::string& name) const;
        virtual const glm::vec3& GetVector3(const std::string& name) const;
        virtual const glm::vec4& GetVector4(const std::string& name) const;
        virtual const glm::ivec2& GetIntVector2(const std::string& name) const;
        virtual const glm::ivec3& GetIntVector3(const std::string& name) const;
        virtual const glm::ivec4& GetIntVector4(const std::string& name) const;
        virtual const glm::mat3& GetMatrix3(const std::string& name) const;
        virtual const glm::mat4& GetMatrix4(const std::string& name) const;

        virtual Ref<Texture2D> GetTexture2D(const std::string& name);
        virtual Ref<Texture2D> GetTexture2D(const std::string& name, u32 arrayIndex);
        virtual Ref<TextureCubemap> GetTextureCube(const std::string& name);

        // Const overloads that forward to the non-const virtuals for backward compatibility
        Ref<Texture2D> GetTexture2D(const std::string& name) const;
        Ref<Texture2D> GetTexture2D(const std::string& name, u32 arrayIndex) const;
        Ref<TextureCubemap> GetTextureCube(const std::string& name) const;

        [[nodiscard]] virtual Ref<Texture2D> TryGetTexture2D(const std::string& name);
        [[nodiscard]] virtual Ref<Texture2D> TryGetTexture2D(const std::string& name, u32 arrayIndex);
        virtual Ref<TextureCubemap> TryGetTextureCube(const std::string& name);

        // Const overloads that forward to the non-const virtuals for backward compatibility
        [[nodiscard]] Ref<Texture2D> TryGetTexture2D(const std::string& name) const;
        [[nodiscard]] Ref<Texture2D> TryGetTexture2D(const std::string& name, u32 arrayIndex) const;
        Ref<TextureCubemap> TryGetTextureCube(const std::string& name) const;

        virtual u32 GetFlags() const
        {
            return m_MaterialFlags;
        }
        virtual void SetFlags(u32 flags)
        {
            m_MaterialFlags = flags;
        }

        virtual bool GetFlag(MaterialFlag flag) const
        {
            return (std::to_underlying(flag) & m_MaterialFlags) != 0;
        }
        virtual void SetFlag(MaterialFlag flag, bool value = true);

        // IBL configuration method
        void ConfigureIBL(const Ref<TextureCubemap>& environmentMap,
                          const Ref<TextureCubemap>& irradianceMap,
                          const Ref<TextureCubemap>& prefilterMap,
                          const Ref<Texture2D>& brdfLutMap);

        // =====================================================================
        // TYPED PROPERTY ACCESSORS (Replacement for public member variables)
        // =====================================================================

        // Legacy material properties (for backward compatibility)
        const glm::vec3& GetAmbient() const
        {
            return m_Ambient;
        }
        void SetAmbient(const glm::vec3& ambient)
        {
            m_Ambient = ambient;
        }
        const glm::vec3& GetDiffuse() const
        {
            return m_Diffuse;
        }
        void SetDiffuse(const glm::vec3& diffuse)
        {
            m_Diffuse = diffuse;
        }
        const glm::vec3& GetSpecular() const
        {
            return m_Specular;
        }
        void SetSpecular(const glm::vec3& specular)
        {
            m_Specular = specular;
        }
        f32 GetShininess() const
        {
            return m_Shininess;
        }
        void SetShininess(f32 shininess)
        {
            m_Shininess = shininess;
        }
        bool IsUsingTextureMaps() const
        {
            return m_UseTextureMaps;
        }
        void SetUseTextureMaps(bool use)
        {
            m_UseTextureMaps = use;
        }
        Ref<Texture2D> GetDiffuseMap() const
        {
            return m_DiffuseMap;
        }
        void SetDiffuseMap(const Ref<Texture2D>& texture)
        {
            m_DiffuseMap = texture;
        }
        Ref<Texture2D> GetSpecularMap() const
        {
            return m_SpecularMap;
        }
        void SetSpecularMap(const Ref<Texture2D>& texture)
        {
            m_SpecularMap = texture;
        }

        // PBR material properties
        const glm::vec4& GetBaseColorFactor() const
        {
            return m_BaseColorFactor;
        }
        void SetBaseColorFactor(const glm::vec4& color)
        {
            m_BaseColorFactor = color;
        }
        const glm::vec4& GetEmissiveFactor() const
        {
            return m_EmissiveFactor;
        }
        void SetEmissiveFactor(const glm::vec4& emissive)
        {
            m_EmissiveFactor = emissive;
        }
        f32 GetMetallicFactor() const
        {
            return m_MetallicFactor;
        }
        void SetMetallicFactor(f32 metallic)
        {
            m_MetallicFactor = metallic;
        }
        f32 GetRoughnessFactor() const
        {
            return m_RoughnessFactor;
        }
        void SetRoughnessFactor(f32 roughness)
        {
            m_RoughnessFactor = roughness;
        }
        f32 GetNormalScale() const
        {
            return m_NormalScale;
        }
        void SetNormalScale(f32 scale)
        {
            m_NormalScale = scale;
        }
        f32 GetOcclusionStrength() const
        {
            return m_OcclusionStrength;
        }
        void SetOcclusionStrength(f32 strength)
        {
            m_OcclusionStrength = strength;
        }
        bool IsIBLEnabled() const
        {
            return m_EnableIBL;
        }
        void SetEnableIBL(bool enable)
        {
            m_EnableIBL = enable;
        }

        AlphaMode GetAlphaMode() const
        {
            return m_AlphaMode;
        }
        void SetAlphaMode(AlphaMode mode)
        {
            m_AlphaMode = mode;
        }
        f32 GetAlphaCutoff() const
        {
            return m_AlphaCutoff;
        }
        void SetAlphaCutoff(f32 cutoff)
        {
            // Sanitize at the API boundary: NaN/Inf would propagate into the
            // UBO and break the shader-side mask discard (which expects a
            // finite [0,1] threshold).
            if (!std::isfinite(cutoff))
                cutoff = 0.0f;
            m_AlphaCutoff = std::clamp(cutoff, 0.0f, 1.0f);
        }

        // =====================================================================
        // PHYSICAL TRANSMISSION / IOR / VOLUME (issue #970)
        //
        // The three glTF extensions KHR_materials_transmission, _ior and
        // _volume, kept as authored so a round-trip returns what the asset
        // said. Every default here is NEUTRAL: TransmissionFactor 0 makes the
        // shader skip the whole closure, IOR 1.5 is exactly the F0 = 0.04 the
        // dielectric path already hardcodes, and an attenuation distance of
        // +infinity with a white attenuation colour means "no absorption".
        // A material that never touches these setters is therefore bit-
        // identical to one from before this feature existed, which
        // MaterialTransmissionTest pins rather than asserts in a comment.
        //
        // The GPU does NOT see AttenuationColor/AttenuationDistance directly:
        // GetAttenuationSigma() derives the Beer-Lambert coefficient once on
        // the CPU, which keeps every infinity out of the UBO and out of GLSL.
        f32 GetTransmissionFactor() const
        {
            return m_TransmissionFactor;
        }
        void SetTransmissionFactor(f32 transmission)
        {
            // Sanitize at the API boundary, as SetAlphaCutoff does: this value
            // arrives straight from glTF/YAML and a NaN would reach the UBO.
            if (!std::isfinite(transmission))
                transmission = 0.0f;
            m_TransmissionFactor = std::clamp(transmission, 0.0f, 1.0f);
        }

        // The one gate the renderer and the serializers agree on. Strictly
        // greater than zero, never an == comparison on a float.
        bool IsTransmissive() const
        {
            return m_TransmissionFactor > 0.0f;
        }

        f32 GetIOR() const
        {
            return m_IOR;
        }
        void SetIOR(f32 ior)
        {
            // glTF allows >= 1.0, plus the special value 0.0 which the spec
            // defines as "no refraction". Anything strictly between 0 and 1 is
            // nonsense, so it falls back to the default rather than producing a
            // negative F0 in the Fresnel term.
            if (!std::isfinite(ior))
                ior = kDefaultIOR;
            m_IOR = (ior > 0.0f && ior < 1.0f) ? kDefaultIOR : std::clamp(ior, 0.0f, kMaxIOR);
        }

        f32 GetThicknessFactor() const
        {
            return m_ThicknessFactor;
        }
        void SetThicknessFactor(f32 thickness)
        {
            if (!std::isfinite(thickness))
                thickness = 0.0f;
            m_ThicknessFactor = std::max(thickness, 0.0f);
        }

        // Thickness is what separates a thin-walled surface (0) from a real
        // volume (> 0). KHR_materials_volume applies its volume half only when
        // thickness is non-zero, so this is the gate the shading path and the
        // compatibility table both use.
        bool HasVolume() const
        {
            return m_ThicknessFactor > 0.0f;
        }

        const glm::vec3& GetAttenuationColor() const
        {
            return m_AttenuationColor;
        }
        void SetAttenuationColor(const glm::vec3& color)
        {
            // Per-channel clamp with a positive floor: a channel of exactly 0
            // is legal glTF but means "infinitely absorbing", and log(0) is
            // -inf, which would make the derived sigma infinite and produce a
            // NaN at thickness 0 (inf * 0). kMinAttenuationChannel keeps the
            // derivation finite while still reading as black over any
            // meaningful thickness.
            glm::vec3 sanitized = color;
            for (int channel = 0; channel < 3; ++channel)
            {
                if (!std::isfinite(sanitized[channel]))
                    sanitized[channel] = 1.0f;
                sanitized[channel] = std::clamp(sanitized[channel], kMinAttenuationChannel, 1.0f);
            }
            m_AttenuationColor = sanitized;
        }

        f32 GetAttenuationDistance() const
        {
            return m_AttenuationDistance;
        }
        void SetAttenuationDistance(f32 distance)
        {
            // +infinity is the glTF default and a MEANINGFUL value here ("no
            // absorption"), so unlike every other setter this one accepts a
            // non-finite input -- but only +inf, never NaN and never -inf.
            if (std::isnan(distance) || distance <= 0.0f)
                distance = std::numeric_limits<f32>::infinity();
            // A FINITE distance also gets a positive floor, or GetAttenuationSigma
            // overflows to +inf in the denormal band -- see kMinAttenuationDistance.
            else if (std::isfinite(distance))
                distance = std::max(distance, kMinAttenuationDistance);
            m_AttenuationDistance = distance;
        }

        // The Beer-Lambert extinction coefficient the shader wants, derived
        // once here instead of per fragment.
        //
        // KHR_materials_volume defines transmittance through a slab of
        // thickness d as
        //     transmittance = exp(log(attenuationColor) * d / attenuationDistance)
        // i.e. exp(-sigma * d) with sigma = -log(attenuationColor) / attenuationDistance.
        //
        // Deriving it on the CPU is what keeps infinity out of the UBO: the
        // default attenuation distance is +inf, and a finite numerator over
        // +inf is exactly 0 in IEEE-754, so the neutral case lands on sigma 0
        // -- exp(0) == 1, no absorption -- without a single inf or NaN
        // crossing into GLSL. Both inputs are sanitized by their setters, so
        // the result is always finite and >= 0.
        glm::vec3 GetAttenuationSigma() const
        {
            glm::vec3 sigma(0.0f);
            for (int channel = 0; channel < 3; ++channel)
                sigma[channel] = -std::log(m_AttenuationColor[channel]) / m_AttenuationDistance;
            return sigma;
        }

        // PBR texture maps
        Ref<Texture2D> GetAlbedoMap() const
        {
            return m_AlbedoMap;
        }
        void SetAlbedoMap(const Ref<Texture2D>& texture)
        {
            m_AlbedoMap = texture;
        }
        Ref<Texture2D> GetMetallicRoughnessMap() const
        {
            return m_MetallicRoughnessMap;
        }
        void SetMetallicRoughnessMap(const Ref<Texture2D>& texture)
        {
            m_MetallicRoughnessMap = texture;
        }
        Ref<Texture2D> GetNormalMap() const
        {
            return m_NormalMap;
        }
        void SetNormalMap(const Ref<Texture2D>& texture)
        {
            m_NormalMap = texture;
        }
        Ref<Texture2D> GetAOMap() const
        {
            return m_AOMap;
        }
        void SetAOMap(const Ref<Texture2D>& texture)
        {
            m_AOMap = texture;
        }
        Ref<Texture2D> GetEmissiveMap() const
        {
            return m_EmissiveMap;
        }
        void SetEmissiveMap(const Ref<Texture2D>& texture)
        {
            m_EmissiveMap = texture;
        }
        Ref<TextureCubemap> GetEnvironmentMap() const
        {
            return m_EnvironmentMap;
        }
        void SetEnvironmentMap(const Ref<TextureCubemap>& texture)
        {
            m_EnvironmentMap = texture;
        }
        Ref<TextureCubemap> GetIrradianceMap() const
        {
            return m_IrradianceMap;
        }
        void SetIrradianceMap(const Ref<TextureCubemap>& texture)
        {
            m_IrradianceMap = texture;
        }
        Ref<TextureCubemap> GetPrefilterMap() const
        {
            return m_PrefilterMap;
        }
        void SetPrefilterMap(const Ref<TextureCubemap>& texture)
        {
            m_PrefilterMap = texture;
        }
        Ref<Texture2D> GetBRDFLutMap() const
        {
            return m_BRDFLutMap;
        }
        void SetBRDFLutMap(const Ref<Texture2D>& texture)
        {
            m_BRDFLutMap = texture;
        }

        // =====================================================================
        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Material;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Accessors for serialization
        const TMap<FString, float>& GetFloatUniforms() const
        {
            return m_FloatUniforms;
        }
        const TMap<FString, int>& GetIntUniforms() const
        {
            return m_IntUniforms;
        }
        const TMap<FString, u32>& GetUIntUniforms() const
        {
            return m_UIntUniforms;
        }
        const TMap<FString, bool>& GetBoolUniforms() const
        {
            return m_BoolUniforms;
        }
        const TMap<FString, glm::vec2>& GetVec2Uniforms() const
        {
            return m_Vec2Uniforms;
        }
        const TMap<FString, glm::vec3>& GetVec3Uniforms() const
        {
            return m_Vec3Uniforms;
        }
        const TMap<FString, glm::vec4>& GetVec4Uniforms() const
        {
            return m_Vec4Uniforms;
        }
        const TMap<FString, glm::ivec2>& GetIVec2Uniforms() const
        {
            return m_IVec2Uniforms;
        }
        const TMap<FString, glm::ivec3>& GetIVec3Uniforms() const
        {
            return m_IVec3Uniforms;
        }
        const TMap<FString, glm::ivec4>& GetIVec4Uniforms() const
        {
            return m_IVec4Uniforms;
        }
        const TMap<FString, glm::mat3>& GetMat3Uniforms() const
        {
            return m_Mat3Uniforms;
        }
        const TMap<FString, glm::mat4>& GetMat4Uniforms() const
        {
            return m_Mat4Uniforms;
        }
        const TMap<FString, Ref<Texture2D>>& GetTexture2DUniforms() const
        {
            return m_Texture2DUniforms;
        }
        const TMap<FString, Ref<TextureCubemap>>& GetTextureCubeUniforms() const
        {
            return m_TextureCubeUniforms;
        }

      protected:
        Material(const Ref<OloEngine::Shader>& shader, const std::string& name = "");

        // Helper method to generate composite keys for array textures
        static std::string GenerateArrayKey(const std::string& name, u32 arrayIndex);

      protected:
        Ref<OloEngine::Shader> m_Shader;
        std::string m_Name;
        u32 m_MaterialFlags = std::to_underlying(MaterialFlag::DepthTest);

        // Material properties storage (uniform system)
        // Using TMap for better cache performance on hot path (every draw call)
        TMap<FString, float> m_FloatUniforms;
        TMap<FString, int> m_IntUniforms;
        TMap<FString, u32> m_UIntUniforms;
        TMap<FString, bool> m_BoolUniforms;
        TMap<FString, glm::vec2> m_Vec2Uniforms;
        TMap<FString, glm::vec3> m_Vec3Uniforms;
        TMap<FString, glm::vec4> m_Vec4Uniforms;
        TMap<FString, glm::ivec2> m_IVec2Uniforms;
        TMap<FString, glm::ivec3> m_IVec3Uniforms;
        TMap<FString, glm::ivec4> m_IVec4Uniforms;
        TMap<FString, glm::mat3> m_Mat3Uniforms;
        TMap<FString, glm::mat4> m_Mat4Uniforms;
        TMap<FString, Ref<Texture2D>> m_Texture2DUniforms;
        TMap<FString, Ref<TextureCubemap>> m_TextureCubeUniforms;

        // =====================================================================
        // PRIVATE MATERIAL PROPERTIES (Encapsulated)
        // =====================================================================

        // Material type
        MaterialType m_MaterialType = MaterialType::PBR;

        // Legacy material properties (for backward compatibility)
        glm::vec3 m_Ambient = glm::vec3(0.2f);
        glm::vec3 m_Diffuse = glm::vec3(0.8f);
        glm::vec3 m_Specular = glm::vec3(1.0f);
        f32 m_Shininess = 32.0f;
        bool m_UseTextureMaps = false;
        Ref<Texture2D> m_DiffuseMap;
        Ref<Texture2D> m_SpecularMap;

        // PBR material properties
        glm::vec4 m_BaseColorFactor = glm::vec4(1.0f); // Base color (albedo) with alpha
        glm::vec4 m_EmissiveFactor = glm::vec4(0.0f);  // Emissive color
        f32 m_MetallicFactor = 0.0f;                   // Metallic factor
        f32 m_RoughnessFactor = 1.0f;                  // Roughness factor
        f32 m_NormalScale = 1.0f;                      // Normal map scale
        f32 m_OcclusionStrength = 1.0f;                // AO strength
        bool m_EnableIBL = false;                      // Enable IBL
        AlphaMode m_AlphaMode = AlphaMode::Opaque;     // glTF-style alpha mode
        f32 m_AlphaCutoff = 0.5f;                      // Threshold for MASK mode discard
        PBRModel m_PBRModel = PBRModel::Legacy;        // Versioned closure (issue #975)

        // Physical transmission / IOR / volume (issue #970). Neutral defaults --
        // see the accessor block above. Any field added here MUST also be added
        // to the hand-written copy constructor AND operator= in Material.cpp:
        // that pair silently kept compiling when alpha mode was added, and
        // copies came back opaque (issue #629).
        f32 m_TransmissionFactor = 0.0f;                // KHR_materials_transmission, 0 = opaque surface
        f32 m_IOR = kDefaultIOR;                        // KHR_materials_ior, 1.5 == the F0 0.04 already assumed
        f32 m_ThicknessFactor = 0.0f;                   // KHR_materials_volume, 0 = thin-walled (no volume)
        glm::vec3 m_AttenuationColor = glm::vec3(1.0f); // KHR_materials_volume, white = no tint
        f32 m_AttenuationDistance =                     // KHR_materials_volume, +inf = no absorption
            std::numeric_limits<f32>::infinity();

        // PBR texture maps
        Ref<Texture2D> m_AlbedoMap;            // Base color texture
        Ref<Texture2D> m_MetallicRoughnessMap; // Metallic-roughness texture (glTF format)
        Ref<Texture2D> m_NormalMap;            // Normal map
        Ref<Texture2D> m_AOMap;                // Ambient occlusion map
        Ref<Texture2D> m_EmissiveMap;          // Emissive map
        Ref<TextureCubemap> m_EnvironmentMap;  // Environment cubemap
        Ref<TextureCubemap> m_IrradianceMap;   // Irradiance cubemap
        Ref<TextureCubemap> m_PrefilterMap;    // Prefiltered environment map
        Ref<Texture2D> m_BRDFLutMap;           // BRDF lookup table
    };

} // namespace OloEngine

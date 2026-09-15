#pragma once

// =============================================================================
// SkinProfile.h — the authored parameter set a skin material points at, and the
// separately versioned algorithm that evaluates it. Issue #1231.
//
// WHY THE PROFILE IS AN ASSET AND NOT FIELDS ON THE MATERIAL. A head, its ears,
// its lips and its eyelids are different materials with different albedo, normal
// and roughness maps, and the same skin. Duplicating six scattering scalars into
// each of them makes "make the skin less waxy" a six-file edit whose sixth file
// is always missed. One asset, many materials, is also what lets the deferred
// path carry a per-pixel PROFILE IDENTITY in a few bits instead of a parameter
// block per pixel.
//
// WHY THE EVALUATION MODEL LIVES HERE AND IS NOT `MaterialKind`. The issue is
// explicit that the KIND of a material must stay distinct from the VERSION of
// the algorithm that evaluates it, and the engine already learned this once:
// `PBRModel` (#975) exists because correcting a BRDF must not silently restate
// every scene. `MaterialKind::Skin` says "this is skin" forever;
// `SkinEvaluationModel` says "this profile was authored against transport
// version N", and #1241's diffusion lands as version 1 without touching a
// single material's kind. See
// docs/adr/0024-material-kind-is-not-the-closure-version.md.
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

namespace OloEngine
{

    // The versioned skin transport algorithm. Append, never renumber — the
    // number is on disk in every .oloskin file and in every scene that names
    // one.
    enum class SkinEvaluationModel : u8
    {
        // What #1231 ships: the diffuse irradiance and the surface specular are
        // evaluated and kept as SEPARATE terms all the way to the composite,
        // then summed. Numerically this equals the single combined term the
        // engine shaded before, which is the point — it is the seam #1241
        // needs, delivered without restating a pixel.
        DiffuseSpecularSplit = 0,

        // What #1241 ships: everything version 0 does, plus the DIFFUSE half is
        // diffused across the surface in screen space by a Burley normalized
        // diffusion kernel built from ScatterColor and ScatterRadiusMM, while
        // the specular half is recombined untouched. See
        // Renderer/SkinDiffusion.h for the maths and
        // docs/guides/skin-diffusion.md for its limits.
        //
        // A profile stays at version 0 until an author moves it. That is the
        // whole reason the version exists: turning diffusion on is an authoring
        // act per profile, not a renderer setting that restates every scene —
        // the renderer setting only decides whether the pass RUNS.
        ScreenSpaceDiffusion = 1,

        Count
    };

    inline constexpr i32 kSkinEvaluationModelCount = static_cast<i32>(SkinEvaluationModel::Count);

    [[nodiscard]] constexpr std::string_view ToString(SkinEvaluationModel model)
    {
        switch (model)
        {
            case SkinEvaluationModel::DiffuseSpecularSplit:
                return "DiffuseSpecularSplit";
            case SkinEvaluationModel::ScreenSpaceDiffusion:
                return "ScreenSpaceDiffusion";
            case SkinEvaluationModel::Count:
                break;
        }
        return "DiffuseSpecularSplit";
    }

    [[nodiscard]] inline constexpr bool IsValidSkinEvaluationModel(i32 value) noexcept
    {
        return value >= 0 && value < kSkinEvaluationModelCount;
    }

    // -------------------------------------------------------------------------
    // Parameter bounds
    // -------------------------------------------------------------------------
    // Every bound is a REJECTION bound, not a taste bound: a value outside it
    // either makes the transport non-finite or is authoring corruption. They are
    // named here so the asset, the YAML reader, the save-game reader and the
    // editor inspector all clamp to the same numbers instead of three of them
    // agreeing and the fourth drifting.

    // Per-channel mean free path, in MILLIMETRES. Human dermis sits around
    // 0.5-2 mm in red and well under 0.2 mm in blue. Zero would divide by zero
    // in any diffusion profile; the upper bound is "a head is not 20 cm of
    // translucent jelly".
    inline constexpr f32 kMinSkinScatterRadiusMM = 1.0e-3f;
    inline constexpr f32 kMaxSkinScatterRadiusMM = 200.0f;

    // Scales the authored thickness (Material::GetThicknessFactor, which is in
    // metres per glTF KHR_materials_volume) into the millimetre space the radii
    // are expressed in. 1000 is the identity conversion m -> mm; an author
    // scales away from it to exaggerate or damp transmission.
    inline constexpr f32 kMinSkinThicknessScale = 0.0f;
    inline constexpr f32 kMaxSkinThicknessScale = 1.0e4f;

    // @brief The authored values themselves — a plain aggregate, deliberately
    //        separate from the Asset that owns them.
    //
    // Separate because the renderer needs to COPY a profile's parameters (into a
    // per-frame slot table, into a test fixture, into a save-game record) and
    // `Asset` derives from `RefCounted`, whose atomic refcount makes the type
    // non-copyable. Splitting the data out is also what lets the finite-
    // validation live in one function that the asset, both deserializers and the
    // editor all call.
    //
    // UNITS AND COLOUR SPACE ARE PART OF THE CONTRACT, not a comment: every
    // colour below is LINEAR (Rec.709 primaries, no transfer function applied),
    // because that is the space the whole lighting stack works in, and every
    // length is MILLIMETRES, because a scattering radius authored in metres is a
    // number no human recognises as wrong. The editor inspector labels them the
    // same way and SkinProfileSerializer writes the same names to YAML.
    struct SkinProfileParameters
    {
        SkinEvaluationModel EvaluationModel = SkinEvaluationModel::DiffuseSpecularSplit;

        // Transport albedo per channel. LINEAR Rec.709, unitless, [0,1]. The
        // fraction of light entering the surface that leaves it again rather
        // than being absorbed — red survives, blue does not, which is what makes
        // skin read as skin. The defaults are a mid-tone dermis: a starting
        // point for authoring, not a measurement.
        glm::vec3 ScatterColor{ 0.85f, 0.55f, 0.45f };

        // Per-channel mean free path, MILLIMETRES.
        glm::vec3 ScatterRadiusMM{ 1.55f, 0.80f, 0.55f };

        // Multiplier taking the material's authored thickness (metres) into the
        // millimetre space of the radii above. Unitless; 1000 is the identity.
        f32 ThicknessScale = 1000.0f;

        // Multiplies the SURFACE specular term of every material using this
        // profile. LINEAR Rec.709, unitless, [0,1]. This is the knob that keeps
        // highlights crisp while the diffuse half is blurred, and it is only
        // meaningful BECAUSE the two outputs are separate — a combined term
        // cannot be tinted without tinting the scattering with it.
        glm::vec3 SpecularTint{ 1.0f, 1.0f, 1.0f };

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected, so
        // callers can log the one case that matters.
        //
        // This is the ONLY validation gate: the YAML reader, the asset-pack
        // reader, the save-game reader and the editor all route through it, so a
        // value cannot reach the GPU by a path that forgot to check it.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinProfileParameters& other) const noexcept;
    };

    // @brief Authored skin scattering parameters, shared by every material that
    //        names this asset.
    class SkinProfile : public Asset
    {
      public:
        SkinProfile() = default;
        // Sanitizes, exactly as SetParameters does. Assigning m_Parameters
        // directly here would have been the one way into this class that skips
        // the validation gate — and SkinProfileTable::Resolve copies whatever it
        // finds without re-checking, so a non-finite value constructed this way
        // would reach the material UBO.
        explicit SkinProfile(const SkinProfileParameters& parameters)
        {
            (void)SetParameters(parameters);
        }
        ~SkinProfile() override = default;

        static AssetType GetStaticType()
        {
            return AssetType::SkinProfile;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Authoring label. Not an identity — the AssetHandle is.
        [[nodiscard]] const std::string& GetName() const noexcept
        {
            return m_Name;
        }
        void SetName(std::string name)
        {
            m_Name = std::move(name);
        }

        [[nodiscard]] const SkinProfileParameters& GetParameters() const noexcept
        {
            return m_Parameters;
        }

        // Assigns and sanitizes in one step. Returns false when a value had to
        // be corrected, so the caller can report it rather than shipping a
        // silently altered profile.
        bool SetParameters(const SkinProfileParameters& parameters);

        // Mutable access for the editor inspector, which edits fields in place
        // through ImGui and calls Sanitize() itself once the widget is done.
        [[nodiscard]] SkinProfileParameters& GetParametersForEdit() noexcept
        {
            return m_Parameters;
        }

        // The parameters a material gets when its handle names nothing loadable.
        // A real, finite, obviously-neutral set rather than a null pointer the
        // shader would have to test for: the caller LOGS and COUNTS the
        // substitution (SkinProfileFallbackReason) so a missing asset is loud,
        // and the frame still renders a head instead of a black hole.
        [[nodiscard]] static SkinProfileParameters DefaultParameters()
        {
            return SkinProfileParameters{};
        }

      private:
        std::string m_Name = "Skin";
        SkinProfileParameters m_Parameters{};
    };

    // -------------------------------------------------------------------------
    // Resolution — and why it is not a silent default
    // -------------------------------------------------------------------------

    // Why a material asked for a profile and did not get the one it named.
    // Mirrors the ShadowTechnique pattern (#1056): a path that cannot do its job
    // says so loudly and countably, because the alternative — quietly shading a
    // head with a neutral profile — looks exactly like a correct frame.
    enum class SkinProfileFallbackReason : u8
    {
        None = 0,       ///< The named profile resolved. No fallback.
        NoHandle,       ///< MaterialKind::Skin with no profile assigned at all.
        AssetMissing,   ///< The handle names an asset the manager cannot load.
        WrongAssetType, ///< The handle resolves to something that is not a SkinProfile.
        SlotBudgetFull, ///< More distinct profiles in one frame than the G-Buffer lane can name.
        MaterialNotPBR, ///< MaterialKind::Skin on a legacy (Phong) material, which has no skin transport.

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(SkinProfileFallbackReason reason)
    {
        switch (reason)
        {
            case SkinProfileFallbackReason::None:
                return "None";
            case SkinProfileFallbackReason::NoHandle:
                return "NoHandle";
            case SkinProfileFallbackReason::AssetMissing:
                return "AssetMissing";
            case SkinProfileFallbackReason::WrongAssetType:
                return "WrongAssetType";
            case SkinProfileFallbackReason::SlotBudgetFull:
                return "SlotBudgetFull";
            case SkinProfileFallbackReason::MaterialNotPBR:
                return "MaterialNotPBR";
            case SkinProfileFallbackReason::Count:
                break;
        }
        return "None";
    }

    // -------------------------------------------------------------------------
    // The per-frame slot table
    // -------------------------------------------------------------------------

    // A skin profile reaches the DEFERRED lighting pass as a small integer in
    // the G-Buffer RT2 flags lane, not as an AssetHandle — 64 bits per pixel is
    // not something a G-Buffer can carry, and the lane has three bits to spare
    // (see oloEncodeGBufferPbrFlags in include/PBRCommon.glsl). So the renderer
    // assigns each DISTINCT profile used in a frame a slot in
    // [0, kMaxSkinProfileSlots).
    //
    // kSkinProfileSlotNone is the "this pixel names no profile" code and is what
    // every non-skin surface writes. It is the all-ones pattern of the field on
    // purpose: a reader must never confuse "nothing written here" with
    // "profile 0".
    inline constexpr u32 kSkinProfileSlotBits = 3;
    inline constexpr u32 kSkinProfileSlotNone = (1u << kSkinProfileSlotBits) - 1u; // 7
    inline constexpr u32 kMaxSkinProfileSlots = kSkinProfileSlotNone;              // slots 0..6

} // namespace OloEngine

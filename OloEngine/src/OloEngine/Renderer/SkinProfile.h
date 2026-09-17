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

        // What #1242 ships: everything version 1 does, plus a THIN-REGION
        // TRANSMISSION term — light entering the far face of a thin region and
        // leaving toward the viewer, attenuated through the authored thickness
        // by the same per-channel mean free paths the diffusion kernel uses.
        // See Renderer/SkinTransmission.h for the maths, the unit chain and the
        // energy argument, and docs/guides/skin-transmission.md for the
        // authoring path and its limits.
        //
        // A VERSION, NOT A FLAG, for the reason version 1 is one: a profile
        // stays where its author left it. Turning transmission on is an
        // authoring act per profile — a head authored against version 1 does not
        // start glowing through its ears because a renderer setting moved, and
        // the renderer setting only decides whether the term is EVALUATED.
        ThicknessTransmission = 2,

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
            case SkinEvaluationModel::ThicknessTransmission:
                return "ThicknessTransmission";
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

    // -------------------------------------------------------------------------
    // Transmission bounds (issue #1242)
    // -------------------------------------------------------------------------

    // Scales the whole transmitted term. 0 disables it for this profile even at
    // transport version 2; 1 is the physical answer. Above 1 would break the
    // energy bound premise 3 of Renderer/SkinTransmission.h's argument rests on,
    // so the ceiling is 1 and is NOT a taste bound — it is the bound
    // SkinTransmissionTest asserts against.
    inline constexpr f32 kMinSkinTransmissionStrength = 0.0f;
    inline constexpr f32 kMaxSkinTransmissionStrength = 1.0f;

    // The forward bias of the exit lobe, `g` in SkinTransmissionLobe. 0 is an
    // isotropic exit (the volume has fully forgotten which way the light came
    // in); 1 is fully view-dependent. Unitless.
    inline constexpr f32 kMinSkinTransmissionAnisotropy = 0.0f;
    inline constexpr f32 kMaxSkinTransmissionAnisotropy = 1.0f;

    // The sharpness of the forward part of the exit lobe, `P` in
    // SkinTransmissionLobe. Below 1 the pow() is a root and the lobe stops being
    // monotone in the useful direction; above 64 it is a specular spike, which a
    // diffuse exit is not.
    inline constexpr f32 kMinSkinTransmissionPower = 1.0f;
    inline constexpr f32 kMaxSkinTransmissionPower = 64.0f;

    // The largest authored thickness, MILLIMETRES, the transmission term will
    // evaluate. Past it the transmittance is below the quantisation of an
    // RGBA16F target for any plausible radiance and the exp() is wasted work;
    // more usefully, it bounds what a corrupt thickness map can ask for. A head
    // is not two metres thick.
    inline constexpr f32 kMaxSkinThicknessMM = 2.0e3f;

    // @brief The transmission half of a skin profile's authored parameters
    //        (issue #1242).
    //
    // A nested aggregate rather than three more fields on SkinProfileParameters
    // so that every function in Renderer/SkinTransmission.h that needs only the
    // lobe shape can take THIS, and the energy bound reads as a property of
    // three numbers instead of of eight. It is still serialized as part of the
    // profile and sanitized by the profile's own Sanitize() — SkinProfile.h
    // promises ONE validation gate and this struct does not open a second.
    struct SkinTransmissionParameters
    {
        // Scales the whole term. Meaningful only at transport version 2
        // (SkinEvaluationModel::ThicknessTransmission); the version branch, not
        // this field, is what stops an older profile acquiring transmission.
        f32 Strength = 1.0f;

        // `g` — how much of the exit lobe is view-dependent.
        f32 Anisotropy = 0.7f;

        // `P` — the sharpness of the view-dependent part.
        f32 Power = 4.0f;

        // Clamp every field into its bound and replace every non-finite value
        // with the default. Returns true when nothing had to be corrected.
        // Called by SkinProfileParameters::Sanitize, never on its own.
        bool Sanitize();

        [[nodiscard]] bool operator==(const SkinTransmissionParameters& other) const noexcept;
    };

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

        // The thin-region transmission lobe (issue #1242). Read only at
        // transport version 2; see SkinTransmissionParameters above and
        // Renderer/SkinTransmission.h.
        SkinTransmissionParameters Transmission{};

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
    // Why a material asked for transmission and did not get it
    // -------------------------------------------------------------------------

    // The house rule on silent fallbacks, applied to the two ways a skin
    // material can fail to transmit. Counted and logged like
    // SkinProfileFallbackReason, and for the same reason: a head that quietly
    // stopped transmitting looks exactly like a head that never should have.
    enum class SkinTransmissionFallbackReason : u8
    {
        None = 0,

        // MaterialKind::Skin with a version-2 profile and NO authored thickness
        // — no thicknessFactor and no thickness map. The conservative answer is
        // no transmission, because the other reading of a zero thickness is
        // "infinitely thin", which renders the uniformly emissive head the
        // issue's second criterion forbids. Documented in
        // docs/guides/skin-transmission.md.
        NoThickness,

        // A thickness map that could not be loaded. Distinct from NoThickness:
        // the author DID author one, so the scalar factor is used alone and the
        // per-pixel variation — the ear — is the thing that went missing.
        ThicknessMapMissing,

        // MaterialKind::Skin with KHR_materials_transmission's TransmissionFactor
        // also raised. Two transmission closures on one surface IS the
        // double-count the fourth criterion forbids, arriving by the authoring
        // path rather than by the maths: skin's term wins, the refractive one is
        // dropped for this material, and the author is told.
        RefractiveTransmissionConflict,

        // The DEFERRED path could not carry this pixel's per-pixel thickness
        // because the surface is lightmapped and RT5 is holding its baked
        // irradiance (see include/SkinTransmission.glsl). The scalar factor is
        // used alone — same loss as ThicknessMapMissing, different cause, and
        // worth telling apart because this one is fixed by unlightmapping the
        // head rather than by fixing an asset.
        DeferredThicknessLaneUnavailable,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(SkinTransmissionFallbackReason reason)
    {
        switch (reason)
        {
            case SkinTransmissionFallbackReason::None:
                return "None";
            case SkinTransmissionFallbackReason::NoThickness:
                return "NoThickness";
            case SkinTransmissionFallbackReason::ThicknessMapMissing:
                return "ThicknessMapMissing";
            case SkinTransmissionFallbackReason::RefractiveTransmissionConflict:
                return "RefractiveTransmissionConflict";
            case SkinTransmissionFallbackReason::DeferredThicknessLaneUnavailable:
                return "DeferredThicknessLaneUnavailable";
            case SkinTransmissionFallbackReason::Count:
                break;
        }
        return "None";
    }

    inline constexpr i32 kSkinTransmissionFallbackReasonCount =
        static_cast<i32>(SkinTransmissionFallbackReason::Count);

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

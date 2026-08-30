#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr f32 kGravity = 9.81f; // matches WaterSurface / WaterCommon.glsl

        // --- The amplitude law -------------------------------------------------
        //
        // All of it lives here, on the CPU, so the two evaluators never have to
        // agree about anything but arithmetic on numbers they were both handed
        // (WaterWake.h section 3).

        /// Fraction of the VELOCITY HEAD (v^2 / 2g) a hull piles up at the bow.
        ///
        /// Expressed as a fraction of the velocity head rather than as a metres-
        /// per-(m/s) constant because that is the quantity that actually scales:
        /// the rise at a stagnation point is v^2 / 2g, and a displacement hull
        /// realises some fraction of it. 0.12 puts a 8 m/s launch's bow wave at
        /// ~39 cm, which reads clearly at the camera distances Drift sails at.
        constexpr f32 kBowVelocityHeadFraction = 0.12f;

        /// Absolute ceiling on the bow rise, and a second ceiling proportional
        /// to the hull's own beam. The beam-relative one is the one that
        /// matters: a bow wave taller than the boat is wide looks like the sea
        /// heaving rather than like the boat pushing, and it is exactly what an
        /// unbounded v^2 term produces the moment somebody cheats the throttle.
        constexpr f32 kMaxBowMetres = 0.45f;
        constexpr f32 kBowBeamFraction = 0.40f;

        /// The stern trough as a fraction of the bow rise. Less than 1 because
        /// a transom drags a shallower depression than the bow raises.
        constexpr f32 kSternTroughFraction = 0.55f;

        constexpr f32 kBowReachLengthFraction = 0.90f;
        constexpr f32 kSternReachLengthFraction = 1.20f;

        /// Metres of arm-ridge height per m/s of hull speed, and its ceiling.
        constexpr f32 kArmHeightPerSpeed = 0.045f;
        constexpr f32 kMaxArmHeightMetres = 0.35f;

        /// How fast an arm ridge decays with age. `1 / (1 + k * age)` rather
        /// than an exponential: an arm that dies exponentially is gone within a
        /// couple of seconds, and the whole point of the historical trail is
        /// that it is still legible several boat-lengths back.
        constexpr f32 kArmHeightDecayPerSecond = 0.50f;

        /// Arm ridge width: a floor from the hull's beam, widening with age
        /// because a real wave train spreads as it propagates. The widening is
        /// also what keeps the far end of the arm above the water mesh's vertex
        /// spacing, so it fades by BandLimit rather than aliasing (WaterWake.h
        /// section 5).
        constexpr f32 kArmRadiusBeamFraction = 0.80f;
        constexpr f32 kMinArmRadiusMetres = 0.50f;
        constexpr f32 kArmRadiusGrowthPerSecond = 0.35f;

        [[nodiscard("the finiteness result must be used")]] bool IsFinite2(glm::vec2 v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y);
        }
    } // namespace

    WaterWakeSystem::WaterWakeData WaterWakeSystem::s_Data;

    void WaterWakeSystem::BeginFrame()
    {
        s_Data.m_HullCount = 0;
        // The vec4 array is deliberately NOT cleared: only the first
        // m_HullCount hulls are ever read, on either side, and zeroing 80 vec4
        // per frame to protect against a read that cannot happen would be
        // ceremony. Reset() does clear it, because there the array outlives the
        // scene that filled it.
    }

    bool WaterWakeSystem::SubmitHull(const WaterWakeHullDesc& desc)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_HullCount >= WaterWake::kMaxHulls)
        {
            ++s_Data.m_DroppedHulls;
            return false;
        }

        // --- Reject, rather than repair, unusable input --------------------
        // A hull with a garbage pose is a bug upstream, and substituting a
        // plausible pose here would hide it while still drawing a wake
        // somewhere wrong.
        if (!IsFinite2(desc.m_CentreXZ) || !IsFinite2(desc.m_ForwardXZ))
            return false;
        if (!std::isfinite(desc.m_HalfBeam) || !std::isfinite(desc.m_HalfLength) ||
            !std::isfinite(desc.m_Speed) || !std::isfinite(desc.m_Gate))
            return false;

        const f32 forwardLen = glm::length(desc.m_ForwardXZ);
        if (!(forwardLen > 1.0e-3f))
            return false; // heading is straight up/down; there is no wake direction
        const glm::vec2 forward = desc.m_ForwardXZ / forwardLen;

        const f32 halfBeam = std::clamp(desc.m_HalfBeam, 0.1f, 30.0f);
        const f32 halfLength = std::clamp(desc.m_HalfLength, 0.1f, 150.0f);
        const f32 gate = std::clamp(desc.m_Gate, 0.0f, 1.0f);
        const f32 speed = std::clamp(desc.m_Speed, -100.0f, 100.0f);

        // --- Bow and stern ------------------------------------------------
        const f32 bowCeiling = std::min(kMaxBowMetres, halfBeam * kBowBeamFraction);
        const f32 bowAmplitude =
            std::clamp(kBowVelocityHeadFraction * speed * speed / (2.0f * kGravity), 0.0f, bowCeiling) * gate;
        const f32 sternAmplitude = bowAmplitude * kSternTroughFraction;
        const f32 bowReach = halfLength * kBowReachLengthFraction;
        const f32 sternReach = halfLength * kSternReachLengthFraction;

        const u32 slot = s_Data.m_HullCount;
        const u32 base = slot * WaterWake::kVec4PerHull;
        glm::vec4* out = s_Data.m_Hulls.data() + base;

        out[WaterWake::kOffsetCentreForward] =
            glm::vec4(desc.m_CentreXZ.x, desc.m_CentreXZ.y, forward.x, forward.y);
        out[WaterWake::kOffsetShape] = glm::vec4(halfBeam, halfLength, bowAmplitude, sternAmplitude);

        // --- The arms -----------------------------------------------------
        // Each sample's arm points are placed by the Kelvin law (WaterWake.h
        // section 4) from THAT sample's own speed and age, which is what makes
        // the V diverge along the trail instead of running parallel to it, and
        // what makes it follow an S-turn: the historical HEADINGS curve, so the
        // offsets are taken along curving starboard vectors.
        const u32 requested = std::min(desc.m_ArmSampleCount, WaterWake::kMaxArmSamples);
        u32 packed = 0;
        // Radius of the largest arm feature, for the bounding circle below.
        f32 maxReach = std::max(bowReach, sternReach);
        for (u32 i = 0; i < requested; ++i)
        {
            const WaterWakeArmSample& s = desc.m_Arms[i];
            if (!IsFinite2(s.m_CentreXZ) || !IsFinite2(s.m_ForwardXZ) ||
                !std::isfinite(s.m_AgeSeconds) || !std::isfinite(s.m_Speed) || !std::isfinite(s.m_Gate))
            {
                // Stop rather than skip. Skipping would join the segment across
                // the hole, drawing one long arm through water the hull never
                // crossed — the same reason BoatWakeSystem breaks its chain.
                break;
            }

            const f32 sampleForwardLen = glm::length(s.m_ForwardXZ);
            if (!(sampleForwardLen > 1.0e-3f))
                break;
            const glm::vec2 sampleForward = s.m_ForwardXZ / sampleForwardLen;
            const glm::vec2 sampleStarboard(-sampleForward.y, sampleForward.x);

            const f32 age = std::clamp(s.m_AgeSeconds, 0.0f, 30.0f);
            const f32 sampleSpeed = std::clamp(s.m_Speed, -100.0f, 100.0f);
            const f32 sampleGate = std::clamp(s.m_Gate, 0.0f, 1.0f);

            // Folding the gate into the SPEED rather than into the offset is
            // what keeps a barely-moving hull's arms tucked against the beam
            // instead of springing outward the instant the gate opens. Written
            // exactly as BoatWakeSystem::ArmOffset writes it, because the foam
            // arm and this ridge must land on the same line.
            const f32 offset = WaterWake::ArmOffset(halfBeam, sampleSpeed * sampleGate, age);
            const f32 radius = std::max(halfBeam * kArmRadiusBeamFraction, kMinArmRadiusMetres) +
                               kArmRadiusGrowthPerSecond * age;
            const f32 amplitude = std::min(kArmHeightPerSpeed * std::abs(sampleSpeed), kMaxArmHeightMetres) *
                                  sampleGate / (1.0f + kArmHeightDecayPerSecond * age);

            const glm::vec2 starboardPoint = s.m_CentreXZ + sampleStarboard * offset;
            const glm::vec2 portPoint = s.m_CentreXZ - sampleStarboard * offset;

            const u32 armBase = WaterWake::kOffsetArms + WaterWake::kArmSampleVec4 * packed;
            out[armBase] = glm::vec4(starboardPoint.x, starboardPoint.y, amplitude, radius);
            out[armBase + 1] = glm::vec4(portPoint.x, portPoint.y, 0.0f, 0.0f);
            ++packed;

            maxReach = std::max(maxReach, glm::length(starboardPoint - desc.m_CentreXZ) + radius);
        }

        // Zero any unpacked arm slot. Unlike the per-frame hull array this
        // MATTERS: the evaluator's loop bound is the packed count, but a stale
        // point left in slot i would be read the moment a later frame packs
        // i + 1 samples for the same slot, and it would be a point from a
        // previous position of a possibly different boat.
        for (u32 i = packed; i < WaterWake::kMaxArmSamples; ++i)
        {
            const u32 armBase = WaterWake::kOffsetArms + WaterWake::kArmSampleVec4 * i;
            out[armBase] = glm::vec4(0.0f);
            out[armBase + 1] = glm::vec4(0.0f);
        }

        // --- The bounding circle ------------------------------------------
        // Centred on the hull rather than on the trail's centroid: it is
        // slightly larger that way, and being slightly large costs a few
        // wasted evaluations while being slightly small clips the far end of
        // the wake off in a way that looks like the trail simply ending.
        const f32 footprintRadius = glm::length(glm::vec2(halfBeam, halfLength)) * WaterWake::kFootprintFadeEnd;
        const f32 boundRadius = std::max(footprintRadius, maxReach);
        out[WaterWake::kOffsetBound] =
            glm::vec4(desc.m_CentreXZ.x, desc.m_CentreXZ.y, boundRadius, 0.0f);

        // control.y is the PER-HULL flatten enable, not the scene's strength:
        // the strength is a setting published on the render path while these
        // records are built on the physics path, so baking it in here would
        // always bake the previous frame's value. It is passed to Evaluate
        // instead. 1 means "this hull suppresses the sea"; a future airborne or
        // fully-submerged hull can drop it without touching the scene setting.
        out[WaterWake::kOffsetControl] =
            glm::vec4(static_cast<f32>(packed), 1.0f, bowReach, sternReach);

        ++s_Data.m_HullCount;
        return true;
    }

    void WaterWakeSystem::SetSettings(const WaterWakeSettings& settings)
    {
        s_Data.m_Settings = settings;
        // Sanitise once, here, for the same reason SubmitHull does: these come
        // from a deserialized component and both evaluators must be able to
        // trust them.
        if (!std::isfinite(s_Data.m_Settings.m_HeightScale))
            s_Data.m_Settings.m_HeightScale = 1.0f;
        s_Data.m_Settings.m_HeightScale = std::clamp(s_Data.m_Settings.m_HeightScale, 0.0f, 4.0f);
        if (!std::isfinite(s_Data.m_Settings.m_FlattenStrength))
            s_Data.m_Settings.m_FlattenStrength = 0.9f;
        s_Data.m_Settings.m_FlattenStrength = std::clamp(s_Data.m_Settings.m_FlattenStrength, 0.0f, 1.0f);
    }

    const WaterWakeSettings& WaterWakeSystem::GetSettings()
    {
        return s_Data.m_Settings;
    }

    u32 WaterWakeSystem::GetHullCount()
    {
        return s_Data.m_HullCount;
    }

    const glm::vec4* WaterWakeSystem::GetHullData()
    {
        return s_Data.m_Hulls.data();
    }

    f32 WaterWakeSystem::GetRenderHeightScale()
    {
        if (!s_Data.m_Settings.m_Enabled || s_Data.m_HullCount == 0u)
            return 0.0f;
        return s_Data.m_Settings.m_HeightScale;
    }

    u32 WaterWakeSystem::GetDroppedHullCount()
    {
        return s_Data.m_DroppedHulls;
    }

    void WaterWakeSystem::Reset()
    {
        s_Data.m_Hulls.fill(glm::vec4(0.0f));
        s_Data.m_HullCount = 0;
        s_Data.m_DroppedHulls = 0;
        s_Data.m_Settings = WaterWakeSettings{};
    }
} // namespace OloEngine

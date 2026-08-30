#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Water/WaterWake.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine
{
    /// One historical hull pose an arm segment is laid from (issue #968).
    ///
    /// Ages are seconds BEFORE the current pose and must be non-decreasing with
    /// index — index 0 is the youngest sample (nearest the hull). Submitting
    /// them out of order does not corrupt anything, it simply draws the arms in
    /// the order given, which is a tangle rather than a V.
    struct WaterWakeArmSample
    {
        glm::vec2 m_CentreXZ{ 0.0f };  ///< hull centre at that moment, absolute world XZ
        glm::vec2 m_ForwardXZ{ 0.0f }; ///< unit horizontal heading at that moment
        f32 m_AgeSeconds = 0.0f;       ///< seconds before the current pose
        f32 m_Speed = 0.0f;            ///< signed forward m/s at that moment
        f32 m_Gate = 0.0f;             ///< how hard this pose was making a wake, [0, 1]
    };

    /// Everything one hull contributes to the water's shape this frame.
    ///
    /// The caller owns the GAMEPLAY judgement (`m_Gate` — is this thing making a
    /// wake, and how hard) and WaterWakeSystem owns the SHAPE that judgement
    /// makes in the water. That split is why a future ship, seaplane or
    /// creature can be a client here without reinventing the amplitude law, and
    /// why BoatWakeSystem does not have to know what a Kelvin angle is.
    struct WaterWakeHullDesc
    {
        glm::vec2 m_CentreXZ{ 0.0f };  ///< current hull centre, absolute world XZ
        glm::vec2 m_ForwardXZ{ 0.0f }; ///< current unit horizontal heading
        f32 m_HalfBeam = 1.2f;         ///< half the hull's width, metres
        f32 m_HalfLength = 3.0f;       ///< half the hull's length, metres
        f32 m_Speed = 0.0f;            ///< signed forward m/s right now
        f32 m_Gate = 0.0f;             ///< current wake strength, [0, 1]
        u32 m_ArmSampleCount = 0;      ///< how many of m_Arms are populated
        std::array<WaterWakeArmSample, WaterWake::kMaxArmSamples> m_Arms{};
    };

    /// Scene-level controls, published each frame from the dominant
    /// WaterComponent exactly like WaterDisturbanceSettings.
    struct WaterWakeSettings
    {
        /// Master switch. Off means the shader publishes the disabled state and
        /// physics sees a flat sea, both from the same zero.
        bool m_Enabled = false;

        // NOTE there is deliberately no m_AffectsPhysics here. The visual-only
        // switch lives on WaterProbe::Volume, read straight off the
        // WaterComponent a body is floating over, because these settings are
        // published on the RENDER path and a headless tick never publishes them
        // — a physics gate here would be permanently closed without a renderer,
        // and every functional test of the physical mode would pass by agreeing
        // that nothing happened. One switch, one place.

        /// Multiplier on the wake height. 0 disables the height AND the hull
        /// flatten, so turning it off cannot leave a footprint pressed into the
        /// sea.
        f32 m_HeightScale = 1.0f;

        /// How much of the ocean displacement the hull footprint removes,
        /// [0, 1]. 1 is dead flat inside the hull; the default leaves a little
        /// life in the water against the topsides rather than a visible
        /// rectangle of calm.
        f32 m_FlattenStrength = 0.9f;
    };

    // =========================================================================
    // WaterWakeSystem — the CPU-owned wake-shape record (issue #968).
    //
    // Holds this frame's packed hull records and the scene's settings. Read by
    // TWO consumers that must not disagree:
    //
    //   * CommandDispatch, which copies the records into WaterUBO for the water
    //     stages to evaluate through WaterWakeCommon.glsl;
    //   * WaterProbe::SampleSurfaceY, which evaluates the SAME records through
    //     WaterWake::Evaluate so buoyancy floats on the surface that is drawn.
    //
    // Deliberately NOT a GPU resource, and deliberately not in the render graph.
    // Everything here is plain CPU state, which is what lets a headless scene
    // tick — no GL context, no renderer — still produce a physically correct
    // wake, and lets WaterWakeParityTest drive both halves without a window.
    // WaterDisturbanceSystem owns a texture because a decaying raster has to
    // live somewhere; this one has nothing to store between frames on purpose.
    //
    // Static singleton, mirroring WaterDisturbanceSystem, and carrying the same
    // obligation: Reset() on scene entry. The records are world-anchored, so a
    // second Play session or a runtime scene switch would otherwise inherit the
    // previous scene's hulls at the same world coordinates for one tick.
    // =========================================================================
    class WaterWakeSystem
    {
      public:
        /// Drop the previous frame's hulls. Called once per tick before any
        /// producer submits.
        ///
        /// A producer that stops running (the boat left the water, the system
        /// was disabled) therefore contributes nothing NEXT frame rather than
        /// leaving its last record standing — which would freeze a wake under a
        /// beached hull until something else happened to overwrite the slot.
        static void BeginFrame();

        /// Pack one hull into the record array.
        ///
        /// THE VALIDATION BOUNDARY (WaterWake.h section 3). Non-finite or
        /// degenerate input is rejected here, returning false, so both
        /// evaluators can stay literal mirrors of each other with no sanitising
        /// of their own. Every geometric decision — arm placement from the
        /// Kelvin law, amplitudes, radii, the bounding circle — is made here,
        /// once.
        ///
        /// Bounded at WaterWake::kMaxHulls; a hull submitted past the cap is
        /// DROPPED and counted, first-come, for the same determinism reason
        /// WaterDisturbanceSystem::SubmitSplat drops rather than evicts.
        ///
        /// @return true if the hull was packed.
        static bool SubmitHull(const WaterWakeHullDesc& desc);

        static void SetSettings(const WaterWakeSettings& settings);
        [[nodiscard]] static const WaterWakeSettings& GetSettings();

        /// Hulls packed this frame, and the packed vec4 records themselves.
        /// The pointer is always valid and always kHullVec4Count long; only the
        /// first GetHullCount() hulls' worth is meaningful.
        [[nodiscard]] static u32 GetHullCount();
        [[nodiscard]] static const glm::vec4* GetHullData();

        /// Height scale for the RENDER path — the settings' scale, or 0 for
        /// every reason the wake could be unusable this frame. Packed
        /// unconditionally by CommandDispatch, so a frame that submitted no
        /// hulls or has the feature off cannot show a stale one.
        [[nodiscard]] static f32 GetRenderHeightScale();

        /// Hulls rejected since the last Reset because the array was full.
        /// Diagnostics and tests; non-zero means some boat's wake is missing.
        [[nodiscard]] static u32 GetDroppedHullCount();

        /// Clear the records, the settings and the drop counter. Must be called
        /// on scene load / runtime start, alongside
        /// WaterDisturbanceSystem::Reset.
        static void Reset();

      private:
        struct WaterWakeData
        {
            std::array<glm::vec4, WaterWake::kHullVec4Count> m_Hulls{};
            u32 m_HullCount = 0;
            u32 m_DroppedHulls = 0;
            WaterWakeSettings m_Settings{};
        };

        static WaterWakeData s_Data;
    };
} // namespace OloEngine

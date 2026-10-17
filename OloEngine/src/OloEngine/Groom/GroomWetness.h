#pragma once

// =============================================================================
// GroomWetness.h — waterline-driven wet coats, issue #1615.
//
// A coat is wet where the water reached it. The water line is a HEIGHT above
// the coat's origin -- world metres, measured on the coat as it stands now --
// so the wet band travels with the animal: a dog that walks up the bank out of
// a lake keeps its wet legs until they dry, rather than drying the instant its
// paws are higher than the lake. A point's height is dot(p, Height) for p in
// groom object space, where Height is the world-up row of the coat's transform
// (GroomWetnessAxis), so the measure is exact however the coat or the body it
// is parented to is rotated or scaled -- the dog's body is turned a quarter
// about Z, and its coat's local +Y is level.
//
// THE STATE (GroomWetnessState) is three numbers the scene advances each tick:
//
//   * Immersion in [0, 1]: rises toward 1 at the soak rate while the coat stands
//     in water, falls toward 0 at the dry rate once it does not. Advanced by
//     WeatherSystem::AdvanceWetness, the weather's own integrator -- one
//     wetting curve in the engine, not two.
//   * SoakedLine: the highest the water has reached on the coat since it was
//     last dry. While the coat dries the wet line RECEDES from it toward the
//     coat's lowest point in proportion to Immersion (GroomWetnessLine), so the
//     back dries first and the legs last -- the reverse of how they got wet.
//   * Rain in [0, 1]: the scene's weather wetness times how much of it the coat
//     takes up, with the coat's own dry rate. Uniform over the coat.
//
// THE FRAME (GroomWetnessFrame) is what the GPU reads: the receding line, the
// band either side of it and the rain. GroomWetnessAt turns a height into a
// wetness, and is the twin of oloGroomWetness in include/GroomWetness.glsl.
//
// WHAT WETNESS DOES, at full wetness (GroomWetResponse):
//
//   * DARKER AND MORE SATURATED. Water in and around the fibre lengthens the
//     path light travels inside it before it escapes -- light the dry fibre's
//     cuticle would have reflected is trapped by the water film and sent back
//     in (Lekner & Dorf 1988; Jensen, Legakis & Dorsey 1999). The fibre's
//     absorption sigma_a is multiplied by Absorption: for an albedo that goes
//     as exp(-c sigma_a) that is albedo^Absorption, and it saturates as it
//     darkens. The default is 1.3, not the "about 2.2" shipped wet fur quotes:
//     through a coat's multiple scattering the power compounds, and at 2.2 a
//     saturated orange coat (the golden dog) went a vivid red instead of a
//     deeper wet gold.
//   * GLOSSIER. The water film fills the cuticle's scales: the longitudinal
//     roughness is scaled by Roughness, so the variance V0 by its square.
//   * NO SOFT SHEEN. The frosted spread a dry coat shows at grazing angles is
//     the light the rough cuticle scatters across azimuths; the azimuthal
//     roughness s is scaled by Azimuthal.
//   * FLATTER, SHORTER, CLUMPED, COMBED TOWARD GRAVITY (GroomWetLocal). Each
//     strand, in its root's bind frame (+Z the skin's normal), is laid toward
//     the way gravity pulls along the skin -- hanging where gravity leaves the
//     skin, lying flat where it presses in -- by Flatten, more on surfaces that
//     face up; shortened by Shorten; and its tip pulled toward its clump's mean
//     growth by Clump, with ApplyGroomCoatShape's t^2 weight. Applied in the
//     deformation every deformed-coat consumer reads (GroomStrandDeform.glsl),
//     never in the cook.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

namespace OloEngine
{
    namespace GroomWetnessLimits
    {
        // The wet/dry edge's half-width. Below a few millimetres it aliases on
        // a strand; above a metre there is no edge.
        constexpr f32 MinBand = 0.005f;
        constexpr f32 MaxBand = 1.0f;
        // Rates in 1/s. 100/s is soaked in one tick.
        constexpr f32 MaxRate = 100.0f;
        // Absorption at full wetness: 1 is "no darker", 8 darkens an albedo of
        // 0.5 to 0.004.
        constexpr f32 MinAbsorption = 1.0f;
        constexpr f32 MaxAbsorption = 8.0f;
        // A line below every coat: "nothing here is soaked".
        constexpr f32 DryLine = -1.0e6f;
    } // namespace GroomWetnessLimits

    /// What full wetness does: albedo^1.3 (see the header for why not the
    /// issue's 2.2), half the roughness, half the azimuthal spread, strands
    /// mostly laid down.
    struct GroomWetResponse
    {
        f32 Absorption = 1.3f;
        f32 Roughness = 0.5f;
        f32 Azimuthal = 0.5f;
        f32 Flatten = 0.6f;
        f32 Shorten = 0.15f;
        f32 Clump = 0.5f;
    };

    /// The coat's wetness this frame, as the GPU reads it.
    struct GroomWetnessFrame
    {
        f32 Rain = 0.0f;                        // uniform wetness from rain, [0, 1]
        f32 Line = GroomWetnessLimits::DryLine; // height above the coat's origin below which it is soaked
        f32 Band = 0.05f;                       // half-width of the edge, metres
        glm::vec3 Height{ 0.0f, 1.0f, 0.0f };   // object point p stands dot(p, Height) metres above the origin

        /// Whether any part of the coat is wet: a dry coat's frame is the
        /// identity, and the shaders' wet paths are skipped for it.
        [[nodiscard]] bool Any() const noexcept
        {
            return Rain > 0.0f || Line > GroomWetnessLimits::DryLine * 0.5f;
        }
    };

    /// Wetness at height `height` above the coat's origin (metres). Twin of
    /// oloGroomWetness.
    [[nodiscard]] f32 GroomWetnessAt(f32 height, const GroomWetnessFrame& frame) noexcept;

    /// How a coat measures height: its origin in the world, and Height, the
    /// world-up row of its transform's linear part -- a point p of groom object
    /// space stands dot(p, Height) metres above the origin. The shaders measure
    /// a strand the same way, on the point as drawn.
    struct GroomWetnessAxis
    {
        glm::vec3 Origin{ 0.0f };
        glm::vec3 Height{ 0.0f, 1.0f, 0.0f };
    };
    [[nodiscard]] GroomWetnessAxis MakeGroomWetnessAxis(const glm::mat4& world) noexcept;

    /// A water surface at world height `worldHeight` as a height above the
    /// coat's origin. Nothing for a non-finite surface.
    [[nodiscard]] std::optional<f32> GroomWetnessLocalHeight(const GroomWetnessAxis& axis, f32 worldHeight) noexcept;

    /// The lowest of a coat's object-space box, `boundsMin` to `boundsMax`, as
    /// a height above its origin: the floor the wet line recedes to.
    [[nodiscard]] f32 GroomWetnessLowest(const GroomWetnessAxis& axis, const glm::vec3& boundsMin,
                                         const glm::vec3& boundsMax) noexcept;
    /// HEAVIER GUIDES, the issue's optional response: the water a soaked strand
    /// holds weighs its guide down. The spring that holds a guide to its groomed
    /// shape, per guide, is weakened by 40 % at full wetness and in proportion
    /// below it -- the wetness at that guide's root, so the dry back of a dog
    /// standing in a pond keeps its spring. Dry (0, or unknown) is 1.
    [[nodiscard]] f32 GroomWetGuideStiffness(f32 wetness) noexcept;

    /// Unit gravity, world -Y, in the object space of a coat placed by `world`.
    [[nodiscard]] glm::vec3 GroomWetGravityObject(const glm::mat4& world) noexcept;

    struct GroomWetnessState
    {
        f32 Immersion = 0.0f;
        f32 SoakedLine = GroomWetnessLimits::DryLine;
        f32 Rain = 0.0f;
    };

    struct GroomWetnessInputs
    {
        /// The water surface's height in coat-local metres, when the coat
        /// stands in water this tick.
        std::optional<f32> Waterline;
        /// The coat's lowest point, coat-local metres.
        f32 Bottom = 0.0f;
        /// Rain wetness the coat is heading for: the weather's surface wetness
        /// times the coat's soak.
        f32 RainTarget = 0.0f;
        f32 SoakRate = 2.0f;
        f32 DryRate = 0.02f;
        f32 DeltaSeconds = 0.0f;
    };

    /// One tick of the wetting and drying. Pure; NaN-safe (a non-finite input
    /// leaves the state where it was).
    void AdvanceGroomWetness(GroomWetnessState& state, const GroomWetnessInputs& inputs) noexcept;

    /// The line the coat is wet below: the soaked line, receded toward `bottom`
    /// as Immersion falls. DryLine once nothing is soaked.
    [[nodiscard]] f32 GroomWetnessLine(const GroomWetnessState& state, f32 bottom) noexcept;

    /// The fibre's wet scales at wetness `wetness`. Twin of oloGroomFibreWet.
    struct GroomWetFibreScales
    {
        f32 SigmaA = 1.0f;
        f32 V0 = 1.0f;
        f32 S = 1.0f;
        /// The exponent an ALBEDO -- a tint, a coat's forwarded or back-scattered
        /// share -- is raised to: the same darkening, stated for a colour.
        f32 AlbedoPower = 1.0f;
    };
    [[nodiscard]] GroomWetFibreScales GroomWetFibre(f32 wetness, const GroomWetResponse& response) noexcept;

    /// One point of a strand, in its root's BIND frame (+Z the skin normal), at
    /// strand parameter `t`, wet. `gravityLocal` is unit gravity in that frame;
    /// `clumpGrowth` the strand's clump mean growth (tip - root) in it, zero for
    /// none. Twin of oloGroomWetLocal in GroomStrandDeform.glsl, operation for
    /// operation.
    [[nodiscard]] glm::vec3 GroomWetLocal(const glm::vec3& local, f32 t, f32 wetness, const glm::vec3& gravityLocal,
                                          const glm::vec3& clumpGrowth, const GroomWetResponse& response) noexcept;

    /// The coat's wetness as every deformed-coat consumer reads it: the
    /// per-frame record of the GPU deformation buffer (GroomGpuDeformation.h),
    /// and what the CPU-deformed path is handed. Frame.w is zero for a dry
    /// coat, and every point then passes through untouched.
    struct GroomDeformWetRecord
    {
        /// xyz = unit gravity in groom object space.
        glm::vec4 Gravity{ 0.0f, -1.0f, 0.0f, 0.0f };
        /// xyz = GroomWetnessFrame::Height: a root at o stands dot(o, xyz)
        /// metres above the coat's origin.
        glm::vec4 Height{ 0.0f, 1.0f, 0.0f, 0.0f };
        /// x = rain, y = soaked line (metres above the origin), z = band, w = 1
        /// when any of the coat is wet.
        glm::vec4 Frame{ 0.0f, GroomWetnessLimits::DryLine, 0.05f, 0.0f };
        /// x = flatten, y = shorten, z = clump, at full wetness.
        glm::vec4 Response{ 0.0f };
    };
    static_assert(sizeof(GroomDeformWetRecord) == 64, "GroomStrandDeform.glsl reads the wet record as four uvec4s");

    /// The record for a coat whose frame, response and object-space gravity
    /// are these. Dry (Frame.w == 0) when no part of the coat is wet.
    [[nodiscard]] GroomDeformWetRecord MakeGroomDeformWetRecord(const GroomWetnessFrame& frame,
                                                                const GroomWetResponse& response,
                                                                const glm::vec3& gravityObject) noexcept;

    /// A bind-local point of the strand whose root is at `origin` with
    /// `rotation` (groom object space) this frame, wet as `wet` says, gathering
    /// toward `clumpGrowth` (bind-local; zero for none). Twin of
    /// GroomStrandDeform.glsl's oloGroomDeformWetLocal: the wetness is the
    /// root's, at the root's height dot(origin, Height).
    [[nodiscard]] glm::vec3 GroomDeformWetLocal(const GroomDeformWetRecord& wet, const glm::vec3& origin,
                                                const glm::quat& rotation, const glm::vec3& clumpGrowth,
                                                const glm::vec3& local, f32 t) noexcept;
} // namespace OloEngine

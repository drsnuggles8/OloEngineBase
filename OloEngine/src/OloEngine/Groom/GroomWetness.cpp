#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomWetness.h"

#include "OloEngine/Atmosphere/WeatherSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // How far above the skin a laid strand still stands: tan(5 degrees). A
        // strand laid exactly flat would sink into the skin it is drawn over at
        // every curve of the body.
        constexpr f32 kWetLift = 0.0874887f;
        // Below this share of gravity along the skin, the slope has no downhill
        // and a strand lies the way it already points.
        constexpr f32 kMinDownhill = 0.05f;

        [[nodiscard]] f32 Saturate(f32 x) noexcept
        {
            return std::clamp(x, 0.0f, 1.0f);
        }
    } // namespace

    f32 GroomWetnessAt(f32 height, const GroomWetnessFrame& frame) noexcept
    {
        const f32 rain = std::isfinite(frame.Rain) ? Saturate(frame.Rain) : 0.0f;
        if (!std::isfinite(height) || !std::isfinite(frame.Line))
        {
            return rain;
        }
        // Clamped as MakeGroomDeformWetRecord and the render request clamp it,
        // so the guides the simulation weakens and the coat that is drawn
        // share one wet edge whatever the authored value.
        const f32 band = std::isfinite(frame.Band)
                             ? std::clamp(frame.Band, GroomWetnessLimits::MinBand, GroomWetnessLimits::MaxBand)
                             : GroomWetnessLimits::MinBand;
        // smoothstep(line - band, line + band, height), spelled out in the
        // shader's order.
        const f32 x = Saturate((height - (frame.Line - band)) / (2.0f * band));
        const f32 above = x * x * (3.0f - (2.0f * x));
        return std::max(1.0f - above, rain);
    }

    GroomWetnessAxis MakeGroomWetnessAxis(const glm::mat4& world) noexcept
    {
        GroomWetnessAxis axis;
        const glm::vec3 origin(world[3]);
        // The world-y ROW of the linear part: world y of (world * p) is
        // origin.y + dot(p, row), for any rotation, scale or shear.
        const glm::vec3 height(world[0][1], world[1][1], world[2][1]);
        if (std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z))
        {
            axis.Origin = origin;
        }
        if (std::isfinite(height.x) && std::isfinite(height.y) && std::isfinite(height.z))
        {
            axis.Height = height;
        }
        return axis;
    }

    std::optional<f32> GroomWetnessLocalHeight(const GroomWetnessAxis& axis, f32 worldHeight) noexcept
    {
        if (!std::isfinite(worldHeight))
        {
            return std::nullopt;
        }
        return worldHeight - axis.Origin.y;
    }

    f32 GroomWetnessLowest(const GroomWetnessAxis& axis, const glm::vec3& boundsMin, const glm::vec3& boundsMax) noexcept
    {
        f32 lowest = std::numeric_limits<f32>::max();
        for (u32 corner = 0; corner < 8u; ++corner)
        {
            const glm::vec3 p((corner & 1u) != 0u ? boundsMax.x : boundsMin.x, (corner & 2u) != 0u ? boundsMax.y : boundsMin.y,
                              (corner & 4u) != 0u ? boundsMax.z : boundsMin.z);
            lowest = std::min(lowest, glm::dot(p, axis.Height));
        }
        return std::isfinite(lowest) ? lowest : 0.0f;
    }

    glm::vec3 GroomWetGravityObject(const glm::mat4& world) noexcept
    {
        // The inverse of the LINEAR part, so a scaled coat's gravity is still a
        // direction: normalised after the map.
        const glm::mat3 linear(world);
        const f32 determinant = glm::determinant(linear);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12f)
        {
            return glm::vec3(0.0f, -1.0f, 0.0f);
        }
        const glm::vec3 gravity = glm::inverse(linear) * glm::vec3(0.0f, -1.0f, 0.0f);
        const f32 length = glm::length(gravity);
        return std::isfinite(length) && length > 1.0e-6f ? gravity / length : glm::vec3(0.0f, -1.0f, 0.0f);
    }

    f32 GroomWetGuideStiffness(f32 wetness) noexcept
    {
        return wetness > 0.0f ? 1.0f - (0.4f * std::min(wetness, 1.0f)) : 1.0f;
    }

    f32 GroomWetnessLine(const GroomWetnessState& state, f32 bottom) noexcept
    {
        if (!(state.Immersion > 0.0f) || !(state.SoakedLine > GroomWetnessLimits::DryLine * 0.5f) ||
            !std::isfinite(bottom))
        {
            return GroomWetnessLimits::DryLine;
        }
        return bottom + ((state.SoakedLine - bottom) * Saturate(state.Immersion));
    }

    void AdvanceGroomWetness(GroomWetnessState& state, const GroomWetnessInputs& inputs) noexcept
    {
        const f32 dt = inputs.DeltaSeconds;
        if (!std::isfinite(dt) || dt <= 0.0f || !std::isfinite(inputs.Bottom))
        {
            return;
        }
        const f32 soak = std::isfinite(inputs.SoakRate) ? std::clamp(inputs.SoakRate, 0.0f, GroomWetnessLimits::MaxRate) : 0.0f;
        const f32 dry = std::isfinite(inputs.DryRate) ? std::clamp(inputs.DryRate, 0.0f, GroomWetnessLimits::MaxRate) : 0.0f;

        const bool immersed = inputs.Waterline.has_value() && std::isfinite(*inputs.Waterline) &&
                              *inputs.Waterline > inputs.Bottom;
        if (immersed)
        {
            // Re-based so the wet line is continuous and heads for the higher of
            // where it already is and the water: a coat that was soaked to its
            // back and wades back in at its knees keeps its wet back, and one
            // that was dry soaks up from its feet to the water line at the soak
            // rate -- water wicking up the fur, not a step.
            const f32 now = std::max(GroomWetnessLine(state, inputs.Bottom), inputs.Bottom);
            const f32 top = std::max(now, *inputs.Waterline);
            const f32 span = top - inputs.Bottom;
            state.Immersion = span > 1.0e-6f ? Saturate((now - inputs.Bottom) / span) : 1.0f;
            state.SoakedLine = top;
            state.Immersion = WeatherSystem::AdvanceWetness(state.Immersion, 1.0f, true, soak, dry, dt);
        }
        else
        {
            state.Immersion = WeatherSystem::AdvanceWetness(state.Immersion, 0.0f, false, soak, dry, dt);
            if (!(state.Immersion > 0.0f))
            {
                state.Immersion = 0.0f;
                state.SoakedLine = GroomWetnessLimits::DryLine;
            }
        }

        const f32 rainTarget = std::isfinite(inputs.RainTarget) ? Saturate(inputs.RainTarget) : 0.0f;
        const f32 rain = std::isfinite(state.Rain) ? state.Rain : 0.0f;
        state.Rain = WeatherSystem::AdvanceWetness(rain, rainTarget, rainTarget > rain, soak, dry, dt);
    }

    GroomWetFibreScales GroomWetFibre(f32 wetness, const GroomWetResponse& response) noexcept
    {
        const f32 w = std::isfinite(wetness) ? Saturate(wetness) : 0.0f;
        GroomWetFibreScales scales;
        scales.SigmaA = 1.0f + ((response.Absorption - 1.0f) * w);
        const f32 roughness = 1.0f + ((response.Roughness - 1.0f) * w);
        scales.V0 = roughness * roughness;
        scales.S = 1.0f + ((response.Azimuthal - 1.0f) * w);
        scales.AlbedoPower = scales.SigmaA;
        return scales;
    }

    glm::vec3 GroomWetLocal(const glm::vec3& local, f32 t, f32 wetness, const glm::vec3& gravityLocal,
                            const glm::vec3& clumpGrowth, const GroomWetResponse& response) noexcept
    {
        if (!(wetness > 0.0f) || !std::isfinite(t))
        {
            return local;
        }
        const f32 w = std::min(wetness, 1.0f);
        if (!(glm::length(local) > 1.0e-7f))
        {
            return local;
        }

        // THE TUFT FIRST, as the coat's shape gathers it: the tip toward its
        // clump's mean growth, weighted t^2 (ApplyGroomCoatShape's weight), so
        // the base stays where it grew.
        glm::vec3 p = local;
        if (glm::length(clumpGrowth) > 1.0e-7f)
        {
            const f32 clump = Saturate(response.Clump * w) * t * t;
            p = p + (((clumpGrowth * t) - p) * clump);
        }

        // THEN LAID DOWN, keeping each point's distance from the root. Where
        // gravity leaves the skin (a belly) the strand hangs off it along
        // gravity; where it presses in (a back) the strand lies along the skin
        // the way the slope runs down -- or, on a level patch, the way it
        // already points -- a few degrees off the surface.
        const f32 hang = std::max(gravityLocal.z, 0.0f);
        const glm::vec2 downhill(gravityLocal.x, gravityLocal.y);
        const f32 downhillLength = glm::length(downhill);
        glm::vec2 direction(1.0f, 0.0f);
        if (downhillLength > kMinDownhill)
        {
            direction = downhill / downhillLength;
        }
        else
        {
            const glm::vec2 own(p.x, p.y);
            const f32 ownLength = glm::length(own);
            if (ownLength > 1.0e-7f)
            {
                direction = own / ownLength;
            }
        }
        const f32 tangential = std::sqrt(std::max(0.0f, 1.0f - (hang * hang)));
        const glm::vec3 target = glm::normalize(glm::vec3(direction * tangential, std::max(hang, kWetLift)));
        // More on what faces up: rain and the water's weight press it down there.
        const f32 facingUp = Saturate(-gravityLocal.z);
        const f32 lie = Saturate(response.Flatten * w * (0.5f + (0.5f * facingUp)));
        const f32 reach = glm::length(p);
        glm::vec3 q = p + (((target * reach) - p) * lie);
        const f32 qLength = glm::length(q);
        q = qLength > 1.0e-7f ? q * (reach / qLength) : p;

        // AND SHORTER: wet fibres stick together along their length.
        return q * (1.0f - std::clamp(response.Shorten * w, 0.0f, 0.9f));
    }

    GroomDeformWetRecord MakeGroomDeformWetRecord(const GroomWetnessFrame& frame, const GroomWetResponse& response,
                                                  const glm::vec3& gravityObject) noexcept
    {
        GroomDeformWetRecord record;
        const f32 gravityLength = glm::length(gravityObject);
        const glm::vec3 gravity = std::isfinite(gravityLength) && gravityLength > 1.0e-6f ? gravityObject / gravityLength
                                                                                          : glm::vec3(0.0f, -1.0f, 0.0f);
        record.Gravity = glm::vec4(gravity, 0.0f);
        const bool heightFinite = std::isfinite(frame.Height.x) && std::isfinite(frame.Height.y) && std::isfinite(frame.Height.z);
        record.Height = glm::vec4(heightFinite ? frame.Height : glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
        record.Frame = glm::vec4(std::isfinite(frame.Rain) ? Saturate(frame.Rain) : 0.0f,
                                 std::isfinite(frame.Line) ? frame.Line : GroomWetnessLimits::DryLine,
                                 std::isfinite(frame.Band) ? std::clamp(frame.Band, GroomWetnessLimits::MinBand,
                                                                        GroomWetnessLimits::MaxBand)
                                                           : GroomWetnessLimits::MinBand,
                                 frame.Any() ? 1.0f : 0.0f);
        const auto unit = [](f32 value, f32 hi)
        { return std::isfinite(value) ? std::clamp(value, 0.0f, hi) : 0.0f; };
        record.Response = glm::vec4(unit(response.Flatten, 1.0f), unit(response.Shorten, 0.9f),
                                    unit(response.Clump, 1.0f), 0.0f);
        return record;
    }

    glm::vec3 GroomDeformWetLocal(const GroomDeformWetRecord& wet, const glm::vec3& origin, const glm::quat& rotation,
                                  const glm::vec3& clumpGrowth, const glm::vec3& local, f32 t) noexcept
    {
        if (!(wet.Frame.w > 0.5f))
        {
            return local;
        }
        GroomWetnessFrame frame;
        frame.Rain = wet.Frame.x;
        frame.Line = wet.Frame.y;
        frame.Band = wet.Frame.z;
        const f32 wetness = GroomWetnessAt(glm::dot(origin, glm::vec3(wet.Height)), frame);
        if (!(wetness > 0.0f))
        {
            return local;
        }
        const glm::vec3 gravityLocal = glm::conjugate(rotation) * glm::vec3(wet.Gravity);
        GroomWetResponse response;
        response.Flatten = wet.Response.x;
        response.Shorten = wet.Response.y;
        response.Clump = wet.Response.z;
        return GroomWetLocal(local, t, wetness, gravityLocal, clumpGrowth, response);
    }
} // namespace OloEngine

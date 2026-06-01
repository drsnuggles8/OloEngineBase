#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::WaterSurface
{
    // =========================================================================
    // CPU mirror of OloEditor/assets/shaders/include/WaterCommon.glsl.
    //
    // Lets gameplay / physics code sample the *same* Gerstner ocean surface the
    // GPU renders, so floating bodies (BuoyancySystem) track the visible waves
    // instead of a flat plane. Keep this in lockstep with
    // `WaterCommon.glsl :: sumGerstnerWaves`: if the shader's wave math changes,
    // this port must change identically. The shared invariants are pinned by
    // WaterSurfaceSamplerTest.cpp (per docs/agent-rules/testing-architecture.md
    // — a CPU/GPU mirror in the same spirit as WaterRenderingTest's
    // `ComputeMaxWaveDisplacementCpu`).
    // =========================================================================

    /// Wave parameters as the water shader's UBO sees them. Mirrors the packing
    /// `WaterComponent::PackWaveDir0/1()` + waveParams produce on the GPU side.
    struct Params
    {
        glm::vec4 m_WaveDir0{ 1.0f, 0.0f, 0.5f, 10.0f };  ///< xy = direction0, z = steepness0, w = wavelength0
        glm::vec4 m_WaveDir1{ 0.7f, 0.7f, 0.3f, 15.0f };  ///< xy = direction1, z = steepness1, w = wavelength1
        f32 m_WaveFrequency = 1.0f;                       ///< global frequency multiplier
        f32 m_WaveAmplitude = 0.5f;                       ///< global amplitude multiplier
        f32 m_WaveSpeed = 1.0f;                           ///< the shader folds Time * WaveSpeed into the phase
        f32 m_PlaneHeight = 0.0f;                         ///< world-space Y of the flat (undisplaced) water plane
    };

    /// Full Gerstner displacement delta (dx, dy, dz) the shader adds to a base
    /// vertex at world (baseXZ.x, planeHeight, baseXZ.y) at raw application time
    /// `rawTime` (seconds — the same `Time::GetTime()` the renderer feeds the
    /// water shader; the WaveSpeed factor is applied internally).
    [[nodiscard]] glm::vec3 SampleDisplacement(const Params& params, glm::vec2 baseXZ, f32 rawTime);

    /// World-space water height of the surface column directly above world
    /// `queryXZ`. Gerstner displaces a base point horizontally as well as
    /// vertically, so this inverts the horizontal shift with a few fixed-point
    /// iterations before reading the vertical displacement. Returns the flat
    /// plane height on any non-finite intermediate (fail-safe for physics).
    [[nodiscard]] f32 SampleHeight(const Params& params, glm::vec2 queryXZ, f32 rawTime);
}

#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

namespace OloEngine::Ocean
{
    class OceanFFTField; // FFT ocean cascade (Renderer/Ocean/OceanFFTField.h) — sampled by SampleHeightFFT.
}

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
        glm::vec4 m_WaveDir0{ 1.0f, 0.0f, 0.5f, 10.0f }; ///< xy = direction0, z = steepness0, w = wavelength0
        glm::vec4 m_WaveDir1{ 0.7f, 0.7f, 0.3f, 15.0f }; ///< xy = direction1, z = steepness1, w = wavelength1
        f32 m_WaveFrequency = 1.0f;                      ///< global frequency multiplier
        f32 m_WaveAmplitude = 0.5f;                      ///< global amplitude multiplier
        f32 m_WaveSpeed = 1.0f;                          ///< the shader folds Time * WaveSpeed into the phase
        f32 m_PlaneHeight = 0.0f;                        ///< world-space Y of the flat (undisplaced) water plane
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

    // =========================================================================
    // FFT ocean sampling (WATER_FUTURE_IMPROVEMENTS.md §5.1).
    //
    // When a WaterComponent renders the Tessendorf FFT ocean instead of summing
    // Gerstner waves, physics must follow *that* surface. This reads the
    // OceanFFTField's retained band-limited CPU proxy so a floating body tracks
    // the rendered FFT crest with no GPU readback — the FFT counterpart to the
    // Gerstner SampleHeight above. The CPU/GPU mirror (the field's spectrum + FFT
    // math) is pinned by OceanFFTSpectrumTest; the buoyancy mapping by
    // WaterSurfaceSamplerTest's FFT cases.
    // =========================================================================

    /// World-space water height of the FFT ocean surface column directly above
    /// world `queryXZ`, mirroring Water.glsl's FFT vertex path
    /// (`displacedPos.y = worldPos.y + disp.y * u_FFTParams.z`):
    /// `planeHeight + field.SampleHeight(queryXZ) * heightScale`. The field's
    /// SampleHeight already inverts the choppy horizontal displacement (so the
    /// height belongs to the column above the query) and returns 0 before its
    /// first evaluation, so an un-built field reads as the flat plane.
    /// `heightScale` is the artist multiplier (WaterComponent::m_FFTHeightScale,
    /// `u_FFTParams.z`). Returns `planeHeight` on any non-finite intermediate
    /// (fail-safe for physics). The field is sampled at world XZ — the same
    /// world-anchored mapping the shader uses (`worldPos.xz * 1/patchSize`).
    [[nodiscard]] f32 SampleHeightFFT(const Ocean::OceanFFTField& field, glm::vec2 queryXZ,
                                      f32 planeHeight, f32 heightScale);

    /// Clamp a raw `WaterComponent::m_FFTHeightScale` (the shader's `u_FFTParams.z`
    /// artist multiplier) to the authoring range, mapping a non-finite value to
    /// the 1.0 default. Single source of truth shared by the renderer (Scene.cpp)
    /// and the buoyancy sampler (BuoyancySystem.cpp) so the rendered FFT crest and
    /// the physics surface can't silently drift apart.
    [[nodiscard]] f32 ClampFFTHeightScale(f32 heightScale);
} // namespace OloEngine::WaterSurface

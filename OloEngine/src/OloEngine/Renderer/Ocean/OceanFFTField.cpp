#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Ocean/OceanFFTField.h"

#include <cmath>
#include <utility>

namespace OloEngine::Ocean
{
    OceanFFTField::H0Key OceanFFTField::MakeH0Key(const SpectrumParams& p)
    {
        H0Key k;
        k.Resolution = p.m_Resolution;
        k.PatchSize = p.m_PatchSize;
        k.WindSpeed = p.m_WindSpeed;
        k.WindDirection = p.m_WindDirection;
        k.Amplitude = p.m_Amplitude;
        k.Gravity = p.m_Gravity;
        k.SmallWaveSuppression = p.m_SmallWaveSuppression;
        k.DirectionalExponent = p.m_DirectionalExponent;
        k.Seed = p.m_Seed;
        k.SpectrumType = static_cast<u32>(p.m_SpectrumType);
        k.JonswapGamma = p.m_JonswapGamma;
        k.JonswapFetch = p.m_JonswapFetch;
        k.CascadeCount = p.m_CascadeCount;
        return k;
    }

    namespace
    {
        // Resolution of the band-limited CPU physics proxy evaluated while the
        // GPU owns the rendered field. 64² keeps SampleHeight() tracking the
        // rendered waves (same h0 band, same phases) at roughly a fifth of the
        // 128²-grid CPU cost — the IFFT work scales with N²·log2(N).
        //
        // Per BAND, so the three-band preset costs three of these per tick
        // rather than one. That is the price of buoyancy following a summed
        // surface; the broad and mid bands are the cheap ones (their spectra
        // are empty above bin ~5, but the IFFT does not know that).
        constexpr u32 kPhysicsProxyResolution = 64u;

        // Decorrelate the per-band Gaussian draws. Band 0 keeps the authored
        // seed exactly, so a single-cascade field is bit-identical to the
        // pre-#969 one; the golden-ratio odd constant is the usual cheap
        // splitmix-style spread for the others.
        [[nodiscard]] constexpr u32 CascadeSeed(u32 baseSeed, u32 cascade) noexcept
        {
            return baseSeed + cascade * 0x9E3779B9u;
        }

        // m_Amplitude is authored as "roughly this many metres of RMS wave
        // height per unit", not as a raw spectral scale — see RegenerateSpectra.
        constexpr f32 kRmsMetresPerAmplitude = 0.3f; // m_Amplitude=3 ⇒ ~0.9 m RMS, ~3 m crests

        constexpr f32 kTwoPi = 6.28318530717958647692f;
    } // namespace

    const DisplacementField& OceanFFTField::GetCascadeField(u32 cascade) const noexcept
    {
        static const DisplacementField s_Empty{};
        if (cascade >= m_Preset.Count || cascade >= kMaxOceanCascades)
            return s_Empty;
        return m_Cascades[cascade].Field;
    }

    void OceanFFTField::RegenerateSpectra(const SpectrumParams& params)
    {
        OLO_PROFILE_FUNCTION();

        // Every band's chain runs at the array resolution — the three fields are
        // layers of one texture array, so they must share a size. The per-band
        // derived resolution (Bands[i].Resolution) is what SIZED that array and
        // is not a second grid: a band-limited spectrum on a larger grid is the
        // same spectrum with the extra bins zero, and its inverse FFT is the
        // exact band-limited reconstruction. See Ocean/OceanCascades.h.
        const u32 N = m_Preset.ArrayResolution;

        // Pass 1: build each band's unit-amplitude, band-limited spectrum and
        // measure the height variance it contributes.
        //
        // The raw Phillips/JONSWAP amplitude is unitless and varies wildly with
        // patch size / wind (a default run yields ~cm-flat heights), so
        // m_Amplitude is normalised to a predictable RMS wave height in metres.
        // With disjoint bands the variances ADD — that is what disjoint means —
        // so ONE common scale over all bands hits the target RMS for the summed
        // surface while leaving the spectrum's relative band energies alone.
        // Scaling each band to the target separately would flatten the spectrum
        // into three equal-energy octaves, which is not an ocean.
        f64 totalVariance = 0.0;
        for (u32 i = 0u; i < m_Preset.Count; ++i)
        {
            const CascadeBand& band = m_Preset.Bands[i];
            Cascade& c = m_Cascades[i];

            SpectrumParams bandParams = params;
            bandParams.m_Resolution = N;
            bandParams.m_PatchSize = band.PatchSize;
            bandParams.m_Seed = CascadeSeed(params.m_Seed, i);
            // Express the authored wind in the BAND'S OWN FRAME, so that
            // sampling at R(θ)·x puts the waves back on the authored world
            // heading. The direction is R(+θ), not R(-θ), and the derivation is
            // worth writing down because both look equally plausible: sampling
            // at c = R(θ)x means a world wave vector k appears in the band's
            // coordinates as R(θ)k, since k·x = k·R(-θ)c = (R(θ)k)·c. Rotating
            // the domain WITHOUT this — or with the sign flipped — is a band
            // whose waves travel across the wind, at 2θ to it in the flipped
            // case. See OceanCascades.h point 3, and
            // OceanCascadeTest.RotatedMidBandStillTravelsWithTheWind.
            c.CosRotation = std::cos(band.DomainRotation);
            c.SinRotation = std::sin(band.DomainRotation);
            bandParams.m_WindDirection =
                RotateVec2(params.m_WindDirection, c.CosRotation, c.SinRotation);
            c.Params = bandParams;

            SpectrumParams unit = bandParams;
            unit.m_Amplitude = 1.0f;
            std::vector<Complex> h0 = GenerateH0(unit);
            ApplyBandLimit(h0, N, band.PatchSize, band.KMin, band.KMax);

            // SPECTRAL DENSITY: each band's amplitude must carry its own bin
            // spacing. THIS IS THE ONE THING A SINGLE-CASCADE FIELD NEVER HAD
            // TO GET RIGHT, and it is invisible to every height-based test.
            //
            // GenerateH0 follows Tessendorf and sets |h0(k)| = sqrt(Phi(k)) with
            // no bin-area factor — sound with one grid, because the missing
            // constant is absorbed into the amplitude scale. Across grids it is
            // not: the spectrum is a DENSITY, so a bin represents Phi(k)·dk^2 of
            // energy, and dk = 2*pi/L differs per band. The fine band's tile is
            // 4.43x smaller, so its k-lattice is 4.43x COARSER and each of its
            // bins stands for 19.6x more of the spectrum than a mid-band bin —
            // while carrying the same amplitude. Left uncorrected the fine band
            // is starved by exactly that factor.
            //
            // Measured at Drift's settings before this line existed: summed
            // height RMS was preserved (0.167 -> 0.170 m, so every height
            // assertion passed) while summed SLOPE RMS fell 38%, because slope
            // is where short waves live. The captures showed a mirror-flat
            // foreground — the whole sea gone, with a green suite.
            // OceanCascadeTest.DiagBandEnergyAndSlopeAtDriftSettings is the pin.
            //
            // Only the RATIO between bands matters; the absolute scale is set by
            // the RMS normalisation in pass 2, which is also why a one-cascade
            // field is unchanged — a single constant factor divides straight
            // back out.
            const f32 binSpacing = kTwoPi / band.PatchSize;
            for (Complex& v : h0)
                v *= binSpacing;

            const DisplacementField ref = EvaluateField(bandParams, h0, 0.0f);
            f64 sumSq = 0.0;
            for (f32 h : ref.m_Height)
                sumSq += static_cast<f64>(h) * h;
            if (!ref.m_Height.empty())
                totalVariance += sumSq / static_cast<f64>(ref.m_Height.size());

            c.H0 = std::move(h0);
        }

        // Pass 2: bake the single common scale into every band. Because it
        // multiplies h0 linearly, height / displacement / normals / Jacobian all
        // stay mutually consistent, within a band and across bands.
        const f32 totalRms = static_cast<f32>(std::sqrt(totalVariance));
        const f32 targetRms = params.m_Amplitude * kRmsMetresPerAmplitude;
        const f32 scale = (totalRms > 1e-6f) ? (targetRms / totalRms) : 0.0f;
        for (u32 i = 0u; i < m_Preset.Count; ++i)
        {
            Cascade& c = m_Cascades[i];
            for (Complex& v : c.H0)
                v *= scale;
            c.GpuH0Dirty = true;
            c.PhysicsH0.clear();
            c.PhysicsResolution = 0u;
        }

        // Bands the preset does not use must not keep a stale field: GetField()
        // and the CPU sampler both loop to m_Preset.Count, but a later Update()
        // that RAISES the count would otherwise inherit one.
        for (u32 i = m_Preset.Count; i < kMaxOceanCascades; ++i)
            m_Cascades[i] = Cascade{};
    }

    void OceanFFTField::Update(const SpectrumParams& params, f32 time, bool uploadToGpu, bool useGpuCompute)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsPowerOfTwo(params.m_Resolution))
        {
            OLO_CORE_WARN("OceanFFTField::Update: resolution {} is not a power of two", params.m_Resolution);
            return;
        }

        m_Params = params;

        // Regenerate the base spectra only when the shape params changed
        // (a slider-moved check, not a math-equality test — H0Key::operator!=
        // is a bit-exact compare of the h0-determining subset).
        const H0Key key = MakeH0Key(params);
        if (!m_HasH0 || key != m_H0Key)
        {
            m_Preset = MakeCascadePreset(params.m_CascadeCount, params.m_PatchSize, params.m_Resolution);
            if (!m_Preset.IsValid())
            {
                OLO_CORE_WARN("OceanFFTField::Update: could not build a cascade preset; skipping");
                return;
            }
            RegenerateSpectra(params);
            m_H0Key = key;
            m_HasH0 = true;
        }

        const u32 N = m_Preset.ArrayResolution;

        // GPU compute butterfly path (§1.2): the layers are generated on the
        // GPU and the retained CPU fields shrink to band-limited physics
        // proxies. Falls back to the CPU reference when compute is unavailable.
        if (uploadToGpu && useGpuCompute)
        {
            bool allAvailable = true;
            for (u32 i = 0u; i < m_Preset.Count; ++i)
            {
                if (!m_Cascades[i].Gpu)
                    m_Cascades[i].Gpu = Ref<OceanFFTGpu>::Create();
                if (!m_Cascades[i].Gpu->IsAvailable())
                {
                    allAvailable = false;
                    break;
                }
            }

            if (allAvailable)
            {
                EnsureTextures(N, m_Preset.Count);
                for (u32 i = 0u; i < m_Preset.Count; ++i)
                {
                    Cascade& c = m_Cascades[i];
                    if (c.GpuH0Dirty)
                    {
                        c.Gpu->SetH0(c.H0, N, m_Preset.Bands[i].PatchSize, params.m_Gravity);
                        c.GpuH0Dirty = false;
                    }
                    c.Gpu->Evaluate(time, params.m_Choppiness, m_DisplacementTex, m_DerivativesTex, i);
                }

                {
                    OLO_PROFILE_SCOPE("OceanFFTField::PhysicsProxyEvaluate");
                    const u32 proxyRes = std::min(N, kPhysicsProxyResolution);
                    for (u32 i = 0u; i < m_Preset.Count; ++i)
                    {
                        Cascade& c = m_Cascades[i];
                        if (c.PhysicsH0.empty() || c.PhysicsResolution != proxyRes)
                        {
                            c.PhysicsH0 = ExtractBandLimitedH0(c.H0, N, proxyRes);
                            c.PhysicsResolution = proxyRes;
                        }
                        SpectrumParams proxyParams = c.Params;
                        proxyParams.m_Resolution = proxyRes;
                        c.Field = EvaluateField(proxyParams, c.PhysicsH0, time);
                    }
                }
                return;
            }
        }

        bool anyValid = false;
        for (u32 i = 0u; i < m_Preset.Count; ++i)
        {
            m_Cascades[i].Field = EvaluateField(m_Cascades[i].Params, m_Cascades[i].H0, time);
            anyValid = anyValid || m_Cascades[i].Field.IsValid();
        }

        if (uploadToGpu && anyValid)
            Upload();
    }

    void OceanFFTField::EnsureTextures(u32 resolution, u32 layers)
    {
        const bool needsCreate = !m_DisplacementTex || m_DisplacementTex->GetWidth() != resolution ||
                                 m_DisplacementTex->GetHeight() != resolution ||
                                 m_DisplacementTex->GetLayers() != layers;
        if (!needsCreate)
            return;

        Texture2DArraySpecification spec;
        spec.Width = resolution;
        spec.Height = resolution;
        spec.Layers = layers;
        spec.Format = Texture2DArrayFormat::RGBA32F;
        spec.GenerateMipmaps = false;
        // EVERY layer is periodic over its own band's patch size, and the water
        // shader addresses them with an unbounded worldXZ * (1/L). The array
        // default is CLAMP_TO_EDGE — right for a shadow cascade, wrong here:
        // clamping would render one tile of ocean and smear its border across
        // the rest of the sea. The single-cascade Texture2D this replaced got
        // REPEAT for free from the GL texture default, which is exactly the
        // kind of inherited invariant a container swap silently drops.
        spec.RepeatWrap = true;
        m_DisplacementTex = Texture2DArray::Create(spec);
        m_DerivativesTex = Texture2DArray::Create(spec);
    }

    void OceanFFTField::Upload()
    {
        OLO_PROFILE_FUNCTION();
        const u32 N = m_Preset.ArrayResolution;
        EnsureTextures(N, m_Preset.Count);
        if (!m_DisplacementTex || !m_DerivativesTex)
            return;

        const sizet count = static_cast<sizet>(N) * N;
        m_DisplacementScratch.resize(count);
        m_DerivativesScratch.resize(count);

        for (u32 layer = 0u; layer < m_Preset.Count; ++layer)
        {
            const DisplacementField& field = m_Cascades[layer].Field;
            if (!field.IsValid() || field.m_Resolution != N)
                continue;

            for (sizet i = 0; i < count; ++i)
            {
                const glm::vec2 disp = field.m_HorizontalDisplacement[i];
                const f32 j = field.m_Jacobian[i];
                const f32 foam = std::clamp(1.0f - j, 0.0f, 1.0f); // J<1 ⇒ folding ⇒ foam
                m_DisplacementScratch[i] = glm::vec4(disp.x, field.m_Height[i], disp.y, foam);

                const glm::vec3 n = field.m_Normal[i];
                m_DerivativesScratch[i] = glm::vec4(n.x, n.y, n.z, j);
            }

            m_DisplacementTex->SetLayerData(layer, m_DisplacementScratch.data(), N, N);
            m_DerivativesTex->SetLayerData(layer, m_DerivativesScratch.data(), N, N);
        }
    }

    u32 OceanFFTField::GetDisplacementTextureID() const
    {
        return m_DisplacementTex ? m_DisplacementTex->GetRendererID() : 0u;
    }

    u32 OceanFFTField::GetDerivativesTextureID() const
    {
        return m_DerivativesTex ? m_DerivativesTex->GetRendererID() : 0u;
    }

    RHI::ResourceHandle OceanFFTField::GetDisplacementTextureHandle() const
    {
        return m_DisplacementTex ? m_DisplacementTex->GetRHIHandle() : RHI::NullResource;
    }

    RHI::ResourceHandle OceanFFTField::GetDerivativesTextureHandle() const
    {
        return m_DerivativesTex ? m_DerivativesTex->GetRHIHandle() : RHI::NullResource;
    }

    OceanFFTField::SurfaceSample OceanFFTField::SampleCascades(glm::vec2 worldXZ) const
    {
        // The CPU half of the sampling contract written out in
        // Ocean/OceanCascades.h. Keep this and
        // include/OceanCascadeCommon.glsl::sampleOceanCascades in step: they
        // are two evaluations of one function, and every one of the four
        // accumulators below has a line-for-line twin over there.
        SurfaceSample out;
        if (!std::isfinite(worldXZ.x) || !std::isfinite(worldXZ.y))
            return out;

        for (u32 i = 0u; i < m_Preset.Count; ++i)
        {
            const Cascade& c = m_Cascades[i];
            const DisplacementField& field = c.Field;
            if (!field.IsValid())
                continue;

            const f32 L = m_Preset.Bands[i].PatchSize;
            if (!(L > 0.0f))
                continue;

            // Rotate INTO the band's sampling domain...
            const glm::vec2 domainXZ = RotateVec2(worldXZ, c.CosRotation, c.SinRotation);

            const u32 N = field.m_Resolution;
            const f32 fN = static_cast<f32>(N);
            f32 gx = domainXZ.x / L * fN;
            f32 gz = domainXZ.y / L * fN;
            gx -= std::floor(gx / fN) * fN;
            gz -= std::floor(gz / fN) * fN;

            const u32 x0 = static_cast<u32>(gx) % N;
            const u32 z0 = static_cast<u32>(gz) % N;
            const u32 x1 = (x0 + 1u) % N;
            const u32 z1 = (z0 + 1u) % N;
            const f32 tx = gx - std::floor(gx);
            const f32 tz = gz - std::floor(gz);

            const sizet i00 = static_cast<sizet>(z0) * N + x0;
            const sizet i10 = static_cast<sizet>(z0) * N + x1;
            const sizet i01 = static_cast<sizet>(z1) * N + x0;
            const sizet i11 = static_cast<sizet>(z1) * N + x1;

            const auto bilerp2 = [&](const auto& v)
            {
                return glm::mix(glm::mix(v[i00], v[i10], tx), glm::mix(v[i01], v[i11], tx), tz);
            };

            out.Height += bilerp2(field.m_Height);

            // ...and rotate the vector quantities BACK out of it. Both the
            // displacement and the gradient are expressed in the band's axes.
            const glm::vec2 horizontal = bilerp2(field.m_HorizontalDisplacement);
            out.Horizontal += RotateVec2(horizontal, c.CosRotation, -c.SinRotation);

            // Slopes add; normals do not. -n.xz / n.y is exact against the
            // normalize(vec3(-sx, 1, -sz)) both producers build the normal with.
            const glm::vec3 n = bilerp2(field.m_Normal);
            const f32 ny = (std::abs(n.y) > 1e-5f) ? n.y : 1.0f;
            out.Slope += RotateVec2(glm::vec2(-n.x / ny, -n.z / ny), c.CosRotation, -c.SinRotation);

            // CLAMP PER TEXEL, THEN INTERPOLATE — not the other way round.
            // The displacement texture stores saturate(1 - J) per texel and the
            // sampler interpolates that, so a CPU side that interpolated the
            // Jacobian first and clamped after would be evaluating a different
            // function wherever J strays outside [0,1] — which is exactly the
            // folding crests foam is about. Measured at 0.006 of divergence
            // before this line was written the other way; caught by
            // OceanFFTGpuContractTest
            // .SummedHeightMatchesBetweenTheCpuSamplerAndTheCascadeTextures,
            // which is the only place the two halves are compared on the same
            // bytes.
            const auto& jac = field.m_Jacobian;
            const auto foamAt = [&jac](sizet i)
            { return std::clamp(1.0f - jac[i], 0.0f, 1.0f); };
            out.Foam += glm::mix(glm::mix(foamAt(i00), foamAt(i10), tx),
                                 glm::mix(foamAt(i01), foamAt(i11), tx), tz);
        }

        out.Foam = std::clamp(out.Foam, 0.0f, 1.0f);
        return out;
    }

    glm::vec2 OceanFFTField::SampleHorizontalBilinear(glm::vec2 worldXZ) const
    {
        return SampleCascades(worldXZ).Horizontal;
    }

    f32 OceanFFTField::SampleHeight(glm::vec2 worldXZ) const
    {
        if (m_Preset.Count == 0u)
            return 0.0f;

        // Invert the SUMMED choppy horizontal displacement: find the base
        // parameter position whose displaced image lands on worldXZ, then read
        // its height. A few fixed-point steps converge because the shift is
        // small vs the patch. Summed, not per-band: the bands displace the same
        // water, so inverting them one at a time would each land on a different
        // column and none of them on the one asked about.
        glm::vec2 base = worldXZ;
        for (int iter = 0; iter < 3; ++iter)
        {
            const glm::vec2 disp = SampleHorizontalBilinear(base);
            const glm::vec2 mapped = base + disp;
            const glm::vec2 err = worldXZ - mapped;
            if (!std::isfinite(err.x) || !std::isfinite(err.y))
                return 0.0f;
            base += err;
        }
        const f32 h = SampleCascades(base).Height;
        return std::isfinite(h) ? h : 0.0f;
    }
} // namespace OloEngine::Ocean

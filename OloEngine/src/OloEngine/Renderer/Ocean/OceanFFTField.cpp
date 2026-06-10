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
        return k;
    }

    void OceanFFTField::Update(const SpectrumParams& params, f32 time, bool uploadToGpu)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsPowerOfTwo(params.m_Resolution))
        {
            OLO_CORE_WARN("OceanFFTField::Update: resolution {} is not a power of two; skipping", params.m_Resolution);
            return;
        }

        m_Params = params;

        // Regenerate the base spectrum only when the shape params changed
        // (a slider-moved check, not a math-equality test — H0Key::operator!=
        // is a bit-exact compare of the h0-determining subset).
        const H0Key key = MakeH0Key(params);
        if (!m_HasH0 || key != m_H0Key)
        {
            // The raw Phillips amplitude is unitless and varies wildly with patch
            // size / wind (a default run yields ~cm-flat heights). Normalise the
            // base spectrum so m_Amplitude maps to a predictable RMS wave height
            // in metres: generate a unit-amplitude spectrum, measure the height
            // RMS once, then bake a constant scale into h0 so every per-frame
            // EvaluateField lands at the target RMS. Because the scale multiplies
            // h0 linearly, height / displacement / normals / Jacobian all stay
            // mutually consistent.
            SpectrumParams unit = params;
            unit.m_Amplitude = 1.0f;
            std::vector<Complex> h0 = GenerateH0(unit);

            const DisplacementField ref = EvaluateField(params, h0, 0.0f);
            f64 sumSq = 0.0;
            for (f32 h : ref.m_Height)
                sumSq += static_cast<f64>(h) * h;
            const f32 rms = ref.m_Height.empty() ? 0.0f
                                                 : static_cast<f32>(std::sqrt(sumSq / static_cast<f64>(ref.m_Height.size())));

            constexpr f32 kRmsMetresPerAmplitude = 0.3f; // m_Amplitude=3 ⇒ ~0.9 m RMS, ~3 m crests
            const f32 targetRms = params.m_Amplitude * kRmsMetresPerAmplitude;
            const f32 scale = (rms > 1e-6f) ? (targetRms / rms) : 0.0f;
            for (Complex& c : h0)
                c *= scale;

            m_H0 = std::move(h0);
            m_H0Key = key;
            m_HasH0 = true;
        }

        m_Field = EvaluateField(params, m_H0, time);

        if (uploadToGpu && m_Field.IsValid())
            Upload();
    }

    void OceanFFTField::EnsureTextures(u32 resolution)
    {
        const bool needsCreate =
            !m_DisplacementTex || m_DisplacementTex->GetWidth() != resolution || m_DisplacementTex->GetHeight() != resolution;
        if (!needsCreate)
            return;

        TextureSpecification spec;
        spec.Width = resolution;
        spec.Height = resolution;
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateMips = false;
        spec.MipLevels = 1;
        m_DisplacementTex = Texture2D::Create(spec);
        m_DerivativesTex = Texture2D::Create(spec);
    }

    void OceanFFTField::Upload()
    {
        OLO_PROFILE_FUNCTION();
        const u32 N = m_Field.m_Resolution;
        EnsureTextures(N);
        if (!m_DisplacementTex || !m_DerivativesTex)
            return;

        const sizet count = static_cast<sizet>(N) * N;
        m_DisplacementScratch.resize(count);
        m_DerivativesScratch.resize(count);

        for (sizet i = 0; i < count; ++i)
        {
            const glm::vec2 disp = m_Field.m_HorizontalDisplacement[i];
            const f32 j = m_Field.m_Jacobian[i];
            const f32 foam = std::clamp(1.0f - j, 0.0f, 1.0f); // J<1 ⇒ folding ⇒ foam
            m_DisplacementScratch[i] = glm::vec4(disp.x, m_Field.m_Height[i], disp.y, foam);

            const glm::vec3 n = m_Field.m_Normal[i];
            m_DerivativesScratch[i] = glm::vec4(n.x, n.y, n.z, j);
        }

        const u32 bytes = static_cast<u32>(count * sizeof(glm::vec4));
        m_DisplacementTex->SetData(m_DisplacementScratch.data(), bytes);
        m_DerivativesTex->SetData(m_DerivativesScratch.data(), bytes);
    }

    u32 OceanFFTField::GetDisplacementTextureID() const
    {
        return m_DisplacementTex ? m_DisplacementTex->GetRendererID() : 0u;
    }

    u32 OceanFFTField::GetDerivativesTextureID() const
    {
        return m_DerivativesTex ? m_DerivativesTex->GetRendererID() : 0u;
    }

    glm::vec2 OceanFFTField::SampleHorizontalBilinear(glm::vec2 worldXZ) const
    {
        if (!m_Field.IsValid() || m_Params.m_PatchSize <= 0.0f)
            return glm::vec2(0.0f);

        const u32 N = m_Field.m_Resolution;
        const f32 fN = static_cast<f32>(N);
        f32 gx = worldXZ.x / m_Params.m_PatchSize * fN;
        f32 gz = worldXZ.y / m_Params.m_PatchSize * fN;
        gx -= std::floor(gx / fN) * fN;
        gz -= std::floor(gz / fN) * fN;

        const u32 x0 = static_cast<u32>(gx) % N;
        const u32 z0 = static_cast<u32>(gz) % N;
        const u32 x1 = (x0 + 1u) % N;
        const u32 z1 = (z0 + 1u) % N;
        const f32 tx = gx - std::floor(gx);
        const f32 tz = gz - std::floor(gz);

        const auto& d = m_Field.m_HorizontalDisplacement;
        const glm::vec2 d00 = d[static_cast<sizet>(z0) * N + x0];
        const glm::vec2 d10 = d[static_cast<sizet>(z0) * N + x1];
        const glm::vec2 d01 = d[static_cast<sizet>(z1) * N + x0];
        const glm::vec2 d11 = d[static_cast<sizet>(z1) * N + x1];
        return glm::mix(glm::mix(d00, d10, tx), glm::mix(d01, d11, tx), tz);
    }

    f32 OceanFFTField::SampleHeight(glm::vec2 worldXZ) const
    {
        if (!m_Field.IsValid())
            return 0.0f;

        // Invert the choppy horizontal displacement: find the base grid point
        // whose displaced image lands on worldXZ, then read its height. A few
        // fixed-point steps converge because the shift is small vs the patch.
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
        const f32 h = SampleHeightBilinear(m_Field, m_Params.m_PatchSize, base);
        return std::isfinite(h) ? h : 0.0f;
    }
} // namespace OloEngine::Ocean

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/CloudNoise.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Texture3D.h"

#include <glad/gl.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace OloEngine
{
    CloudNoise::CloudNoiseData CloudNoise::s_Data;

    namespace
    {
        constexpr u32 kBaseNoiseSize = 128;
        constexpr u32 kDetailNoiseSize = 32;
        constexpr u32 kWeatherMapSize = 512;
        // Must match local_size_x/y/z in CloudNoise_Generate.comp
        constexpr u32 kLocalSize = 4;
        // Fixed seed — the default weather map is fully deterministic
        constexpr u32 kWeatherMapSeed = 0x633C10DDu;

        // ---- Deterministic CPU value noise (integer hash — no std::rand,
        // no transcendentals, so the output is bit-identical everywhere) ----

        /// 2D integer-lattice hash → [0, 1).
        [[nodiscard]] f32 HashLattice(u32 x, u32 y, u32 seed)
        {
            u32 h = seed;
            h ^= x * 0x85EBCA6Bu;
            h = (h ^ (h >> 13)) * 0xC2B2AE35u;
            h ^= y * 0x27D4EB2Fu;
            h = (h ^ (h >> 16)) * 0x7FEB352Du;
            h ^= h >> 15;
            // Top 24 bits → exactly representable in f32
            return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
        }

        /// Tiling 2D value noise: (u, v) in [0, 1), lattice wraps at `period`.
        [[nodiscard]] f32 ValueNoise2D(f32 u, f32 v, u32 period, u32 seed)
        {
            const f32 x = u * static_cast<f32>(period);
            const f32 y = v * static_cast<f32>(period);
            u32 x0 = static_cast<u32>(x); // u, v >= 0 → truncation == floor
            u32 y0 = static_cast<u32>(y);
            const f32 fx = x - static_cast<f32>(x0);
            const f32 fy = y - static_cast<f32>(y0);
            const u32 x1 = (x0 + 1) % period;
            const u32 y1 = (y0 + 1) % period;
            x0 %= period;
            y0 %= period;

            // Smooth Hermite fade
            const f32 sx = fx * fx * (3.0f - 2.0f * fx);
            const f32 sy = fy * fy * (3.0f - 2.0f * fy);

            const f32 a = HashLattice(x0, y0, seed);
            const f32 b = HashLattice(x1, y0, seed);
            const f32 c = HashLattice(x0, y1, seed);
            const f32 d = HashLattice(x1, y1, seed);

            const f32 top = a + (b - a) * sx;
            const f32 bottom = c + (d - c) * sx;
            return top + (bottom - top) * sy;
        }

        /// 4-octave tiling FBM, amplitudes normalized so the mean is ~0.5.
        [[nodiscard]] f32 Fbm2D(f32 u, f32 v, u32 basePeriod, u32 seed)
        {
            f32 sum = 0.0f;
            f32 amplitude = 0.5f;
            f32 total = 0.0f;
            u32 period = basePeriod;
            for (u32 octave = 0; octave < 4; ++octave)
            {
                sum += amplitude * ValueNoise2D(u, v, period, seed + octave * 0x9E3779B9u);
                total += amplitude;
                amplitude *= 0.5f;
                period *= 2;
            }
            return sum / total;
        }

        /// Dispatch one bake pass of CloudNoise_Generate.comp into a 3D volume.
        void DispatchNoiseBake(const ComputeShader& shader, u32 textureID, int mode, u32 size)
        {
            shader.SetInt("u_Mode", mode);
            shader.SetInt("u_Size", static_cast<int>(size));
            shader.SetFloat("u_InvSize", 1.0f / static_cast<f32>(size));

            // Bind the volume for writing (image unit 0, mip 0, layered for 3D)
            RenderCommand::BindImageTexture(0, textureID, 0, true, 0, GL_WRITE_ONLY, GL_RGBA8);

            const u32 groups = (size + kLocalSize - 1) / kLocalSize;
            RenderCommand::DispatchCompute(groups, groups, groups);

            // Consumers sample the volume as a texture; image stores must land first
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        }
    } // namespace

    bool CloudNoise::EnsureGenerated()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_Generated)
        {
            return true;
        }
        if (s_Data.m_GenerationFailed)
        {
            return false; // already logged; Shutdown() clears the latch
        }

        // --- 3D noise volumes (RGBA8, repeat so the raymarch tiles them) ---
        Texture3DSpecification baseSpec;
        baseSpec.Width = kBaseNoiseSize;
        baseSpec.Height = kBaseNoiseSize;
        baseSpec.Depth = kBaseNoiseSize;
        baseSpec.Format = Texture3DFormat::RGBA8;
        baseSpec.Repeat = true;
        s_Data.m_BaseNoise = Texture3D::Create(baseSpec);

        Texture3DSpecification detailSpec = baseSpec;
        detailSpec.Width = kDetailNoiseSize;
        detailSpec.Height = kDetailNoiseSize;
        detailSpec.Depth = kDetailNoiseSize;
        s_Data.m_DetailNoise = Texture3D::Create(detailSpec);

        // --- Generation compute shader ---
        s_Data.m_GenerateShader = ComputeShader::Create("assets/shaders/compute/CloudNoise_Generate.comp");

        const bool texturesValid = s_Data.m_BaseNoise && s_Data.m_BaseNoise->GetRendererID() != 0 &&
                                   s_Data.m_DetailNoise && s_Data.m_DetailNoise->GetRendererID() != 0;
        const bool shaderValid = s_Data.m_GenerateShader && s_Data.m_GenerateShader->IsValid();
        if (!texturesValid || !shaderValid)
        {
            OLO_CORE_ERROR("CloudNoise::EnsureGenerated failed — {}",
                           !shaderValid ? "CloudNoise_Generate.comp could not be loaded/compiled"
                                        : "3D noise textures could not be created");
            s_Data.m_GenerateShader = nullptr;
            s_Data.m_BaseNoise = nullptr;
            s_Data.m_DetailNoise = nullptr;
            s_Data.m_GenerationFailed = true;
            return false;
        }

        // --- Bake both volumes on the GPU ---
        s_Data.m_GenerateShader->Bind();
        DispatchNoiseBake(*s_Data.m_GenerateShader, s_Data.m_BaseNoise->GetRendererID(), 0, kBaseNoiseSize);
        DispatchNoiseBake(*s_Data.m_GenerateShader, s_Data.m_DetailNoise->GetRendererID(), 1, kDetailNoiseSize);
        s_Data.m_GenerateShader->Unbind();

        // --- Default weather map (deterministic CPU value-noise FBM) ---
        // R = coverage, G = cloud type (decorrelated FBM), B = wetness (0),
        // A = 255. The coverage field is normalized against its OWN measured
        // min/max: the raw FBM's mean sits well above 0.5 (found live via a
        // shader probe — issue #633: every sky rendered as a solid veil
        // because R never approached 0), so a fixed-midpoint contrast stretch
        // saturates. Full-range normalization guarantees genuinely clear and
        // genuinely cloudy regions regardless of the FBM's true statistics.
        std::vector<u32> pixels(static_cast<sizet>(kWeatherMapSize) * kWeatherMapSize);
        std::vector<f32> coverageField(static_cast<sizet>(kWeatherMapSize) * kWeatherMapSize);
        constexpr f32 kInvSize = 1.0f / static_cast<f32>(kWeatherMapSize);
        f32 coverageMin = std::numeric_limits<f32>::max();
        f32 coverageMax = std::numeric_limits<f32>::lowest();
        for (u32 y = 0; y < kWeatherMapSize; ++y)
        {
            for (u32 x = 0; x < kWeatherMapSize; ++x)
            {
                const f32 u = (static_cast<f32>(x) + 0.5f) * kInvSize;
                const f32 v = (static_cast<f32>(y) + 0.5f) * kInvSize;
                const f32 raw = Fbm2D(u, v, 4, kWeatherMapSeed);
                coverageField[static_cast<sizet>(y) * kWeatherMapSize + x] = raw;
                coverageMin = std::min(coverageMin, raw);
                coverageMax = std::max(coverageMax, raw);
            }
        }
        const f32 coverageRange = std::max(coverageMax - coverageMin, 1.0e-6f);
        for (u32 y = 0; y < kWeatherMapSize; ++y)
        {
            for (u32 x = 0; x < kWeatherMapSize; ++x)
            {
                const sizet idx = static_cast<sizet>(y) * kWeatherMapSize + x;
                const f32 coverage = (coverageField[idx] - coverageMin) / coverageRange;

                const f32 u = (static_cast<f32>(x) + 0.5f) * kInvSize;
                const f32 v = (static_cast<f32>(y) + 0.5f) * kInvSize;
                const f32 cloudType = std::clamp(Fbm2D(u, v, 3, kWeatherMapSeed ^ 0x51ED270Bu), 0.0f, 1.0f);

                const auto r = static_cast<u32>(coverage * 255.0f + 0.5f);
                const auto g = static_cast<u32>(cloudType * 255.0f + 0.5f);
                // Byte order R | G<<8 | B<<16 | A<<24 (matches RGBA8 upload)
                pixels[idx] = (255u << 24u) | (g << 8u) | r;
            }
        }

        TextureSpecification weatherSpec;
        weatherSpec.Width = kWeatherMapSize;
        weatherSpec.Height = kWeatherMapSize;
        weatherSpec.Format = ImageFormat::RGBA8;
        weatherSpec.GenerateMips = false;
        s_Data.m_WeatherMap = Texture2D::Create(weatherSpec);
        if (!s_Data.m_WeatherMap || s_Data.m_WeatherMap->GetRendererID() == 0)
        {
            OLO_CORE_ERROR("CloudNoise::EnsureGenerated failed — default weather map texture could not be created");
            s_Data.m_GenerateShader = nullptr;
            s_Data.m_BaseNoise = nullptr;
            s_Data.m_DetailNoise = nullptr;
            s_Data.m_WeatherMap = nullptr;
            s_Data.m_GenerationFailed = true;
            return false;
        }
        s_Data.m_WeatherMap->SetData(pixels.data(),
                                     static_cast<u32>(pixels.size() * sizeof(u32)));

        s_Data.m_Generated = true;
        OLO_CORE_INFO("CloudNoise generated (128^3 base + 32^3 detail RGBA8 noise, 512^2 default weather map)");
        return true;
    }

    void CloudNoise::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        const bool hadState = s_Data.m_Generated || s_Data.m_GenerationFailed;

        s_Data.m_GenerateShader = nullptr;
        s_Data.m_BaseNoise = nullptr;
        s_Data.m_DetailNoise = nullptr;
        s_Data.m_WeatherMap = nullptr;
        s_Data.m_Generated = false;
        s_Data.m_GenerationFailed = false;

        if (hadState)
        {
            OLO_CORE_INFO("CloudNoise shut down");
        }
    }

    bool CloudNoise::IsReady()
    {
        return s_Data.m_Generated;
    }

    u32 CloudNoise::GetBaseNoiseTextureID()
    {
        return (s_Data.m_Generated && s_Data.m_BaseNoise) ? s_Data.m_BaseNoise->GetRendererID() : 0;
    }

    u32 CloudNoise::GetDetailNoiseTextureID()
    {
        return (s_Data.m_Generated && s_Data.m_DetailNoise) ? s_Data.m_DetailNoise->GetRendererID() : 0;
    }

    u32 CloudNoise::GetDefaultWeatherMapTextureID()
    {
        return (s_Data.m_Generated && s_Data.m_WeatherMap) ? s_Data.m_WeatherMap->GetRendererID() : 0;
    }
} // namespace OloEngine

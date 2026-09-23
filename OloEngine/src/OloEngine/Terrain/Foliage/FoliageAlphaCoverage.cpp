#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageAlphaCoverage.h"

#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace OloEngine::FoliageAlphaCoverage
{
    namespace
    {
        // A full-avalanche integer hash (lowbias32). Written out rather than
        // taken from <random>: the standard distributions are implementation
        // defined, and the surface samples have to be the same points on
        // MSVC, clang-cl and libstdc++ or a coverage figure near a band edge
        // would warn on one platform and not another.
        [[nodiscard]] constexpr u32 Hash(u32 x) noexcept
        {
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            x *= 0x846ca68bu;
            x ^= x >> 16;
            return x;
        }

        // [0, 1) from the top 24 bits, exactly representable in an f32.
        [[nodiscard]] constexpr f32 Unit(u32 h) noexcept
        {
            return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
        }
    } // namespace

    Band PlausibleBand(Role role) noexcept
    {
        // The rationale for every number is in the header, next to the
        // measurements it was chosen between.
        switch (role)
        {
            case Role::Card:
                return { 0.02f, 0.90f };
            case Role::AuthoredMesh:
            case Role::ImpostorBake:
                return { 0.30f, 1.0f };
        }
        return {};
    }

    Verdict Judge(Role role, f32 passFraction) noexcept
    {
        const Band band = PlausibleBand(role);
        if (passFraction < band.Min)
            return Verdict::TooSparse;
        if (passFraction > band.Max)
            return Verdict::TooSolid;
        return Verdict::Plausible;
    }

    const char* RoleName(Role role) noexcept
    {
        switch (role)
        {
            case Role::Card:
                return "card";
            case Role::AuthoredMesh:
                return "authored mesh";
            case Role::ImpostorBake:
                return "impostor bake";
        }
        return "unknown";
    }

    u8 AlphaPlane::AlphaAt(glm::vec2 uv) const noexcept
    {
        if (IsEmpty())
            return 255;
        // REPEAT wrap. A non-finite UV samples texel (0, 0) rather than
        // indexing with a NaN.
        const f32 u = std::isfinite(uv.x) ? uv.x - std::floor(uv.x) : 0.0f;
        const f32 v = std::isfinite(uv.y) ? uv.y - std::floor(uv.y) : 0.0f;
        const u32 x = std::min(static_cast<u32>(u * static_cast<f32>(Width)), Width - 1);
        // v runs UP the image once Texture2D has flipped it for upload, so
        // the texel row counted from the bottom is floor(v * H), and this
        // plane is stored top-down.
        const u32 rowFromBottom = std::min(static_cast<u32>(v * static_cast<f32>(Height)), Height - 1);
        const u32 row = Height - 1 - rowFromBottom;
        return Alpha[static_cast<i32>(static_cast<sizet>(row) * Width + x)];
    }

    bool DecodeAlpha(const std::filesystem::path& path, AlphaPlane& out, std::string& error)
    {
        OLO_PROFILE_FUNCTION();

        out = AlphaPlane{};
        // Explicitly un-flipped. The thread-local flag latches once anything on
        // this thread has set it (see OpenGLTexture2D's constructor), so
        // relying on the default would make the row order depend on what
        // loaded before.
        ::stbi_set_flip_vertically_on_load_thread(0);
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = ::stbi_load(path.string().c_str(), &width, &height, &channels, 0);
        if (!pixels)
        {
            const char* reason = ::stbi_failure_reason();
            error = reason ? reason : "stb_image could not decode the file";
            return false;
        }
        if (width <= 0 || height <= 0 || channels <= 0)
        {
            ::stbi_image_free(pixels);
            error = "the image has no texels";
            return false;
        }

        out.Width = static_cast<u32>(width);
        out.Height = static_cast<u32>(height);
        const sizet texels = static_cast<sizet>(out.Width) * out.Height;
        out.Alpha.SetNumUninitialized(static_cast<i32>(texels));
        if (channels == 4)
        {
            for (sizet i = 0; i < texels; ++i)
                out.Alpha[static_cast<i32>(i)] = pixels[i * 4 + 3];
        }
        else
        {
            // R8 / RG8 / RGB8 uploads sample alpha as 1.
            std::fill_n(out.Alpha.GetData(), texels, u8{ 255 });
        }
        ::stbi_image_free(pixels);
        return true;
    }

    f32 Histogram::PassFraction(f32 cutoff) const noexcept
    {
        if (IsEmpty())
            return 0.0f;
        f64 passed = 0.0;
        for (u32 bin = 0; bin < 256; ++bin)
        {
            // The shader's own comparison, negated: `alpha < cutoff` discards.
            const f32 alpha = static_cast<f32>(bin) / 255.0f;
            if (!(alpha < cutoff))
                passed += Weight[bin];
        }
        return static_cast<f32>(passed / Total);
    }

    Histogram MeasureSheet(const AlphaPlane& plane)
    {
        Histogram histogram;
        for (const u8 alpha : plane.Alpha)
            histogram.Add(alpha);
        return histogram;
    }

    Histogram MeasureAtUVs(const AlphaPlane& plane, std::span<const glm::vec2> uvs)
    {
        Histogram histogram;
        if (plane.IsEmpty())
            return histogram;
        for (const glm::vec2& uv : uvs)
            histogram.Add(plane.AlphaAt(uv));
        return histogram;
    }

    std::string Describe(const Entry& entry, std::string_view layerName, f32 cutoff)
    {
        if (!entry.Measured)
            return {};
        const f32 fraction = entry.Coverage.PassFraction(cutoff);
        const Verdict verdict = Judge(entry.Kind, fraction);
        if (verdict == Verdict::Plausible)
            return {};

        const Band band = PlausibleBand(entry.Kind);
        const f32 percent = 100.0f * fraction;
        // Every line ends by saying it changed nothing: an author reading it
        // must not go looking for a clamp or a refused layer.
        constexpr std::string_view kDiagnosticOnly = " (Diagnostic only; the layer renders exactly as authored.)";
        std::string text;
        switch (entry.Kind)
        {
            case Role::Card:
                if (verdict == Verdict::TooSolid)
                {
                    text = std::format("FoliageRenderer: layer '{}' draws its card with '{}', which passes {:.1f}% of "
                                       "its texels at AlphaCutoff {:.2f} - above the {:.0f}% a billboard can pass "
                                       "without drawing as a solid rectangle. Is Albedo Path the plant's source "
                                       "atlas, or an image with no alpha, rather than a billboard of the plant?",
                                       layerName, entry.Texture.ToView(), percent, cutoff, 100.0f * band.Max);
                }
                else
                {
                    text = std::format("FoliageRenderer: layer '{}' draws its card with '{}', which passes only {:.1f}% "
                                       "of its texels at AlphaCutoff {:.2f} - a billboard needs at least {:.0f}% to "
                                       "draw more than a few stray pixels. Lower the cutoff, or check the texture's "
                                       "alpha channel.",
                                       layerName, entry.Texture.ToView(), percent, cutoff, 100.0f * band.Min);
                }
                break;
            case Role::AuthoredMesh:
                text = std::format("FoliageRenderer: layer '{}' draws {} with '{}', and only {:.1f}% of that SURFACE "
                                   "passes at AlphaCutoff {:.2f} - solid plant geometry should pass at least {:.0f}%, "
                                   "so the plant draws see-through. Is this the texture the mesh was modelled for?",
                                   layerName, entry.Surface.ToView(), entry.Texture.ToView(), percent, cutoff,
                                   100.0f * band.Min);
                break;
            case Role::ImpostorBake:
                text = std::format("FoliageRenderer: layer '{}' bakes {} into its impostor with '{}', and only "
                                   "{:.1f}% of that surface passes at AlphaCutoff {:.2f} - solid plant geometry should "
                                   "pass at least {:.0f}%, so the far impostor is baked mostly empty there. Is this "
                                   "the texture the mesh was modelled for?",
                                   layerName, entry.Surface.ToView(), entry.Texture.ToView(), percent, cutoff,
                                   100.0f * band.Min);
                break;
        }
        text += kDiagnosticOnly;
        return text;
    }

    TArray<glm::vec2> SampleSurfaceUVs(std::span<const Vertex> vertices, std::span<const u32> indices, u32 sampleCount)
    {
        OLO_PROFILE_FUNCTION();

        TArray<glm::vec2> uvs;
        if (sampleCount == 0 || vertices.empty() || indices.size() < 3)
            return uvs;

        // Running area total per usable triangle, and where that triangle
        // starts in `indices`. Area in f64: a 25k-triangle canopy summed in
        // f32 loses the small triangles' share to rounding.
        TArray<f64> cumulative;
        TArray<u32> firstIndex;
        const sizet triangleCount = indices.size() / 3;
        cumulative.Reserve(static_cast<i32>(triangleCount));
        firstIndex.Reserve(static_cast<i32>(triangleCount));
        const sizet vertexCount = vertices.size();
        f64 total = 0.0;
        for (sizet t = 0; t < triangleCount; ++t)
        {
            const u32 i0 = indices[t * 3 + 0];
            const u32 i1 = indices[t * 3 + 1];
            const u32 i2 = indices[t * 3 + 2];
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                continue;
            const glm::vec3 edge1 = vertices[i1].Position - vertices[i0].Position;
            const glm::vec3 edge2 = vertices[i2].Position - vertices[i0].Position;
            const f64 area = 0.5 * static_cast<f64>(glm::length(glm::cross(edge1, edge2)));
            if (!(area > 0.0) || !std::isfinite(area))
                continue;
            total += area;
            cumulative.Add(total);
            firstIndex.Add(static_cast<u32>(t * 3));
        }
        if (!(total > 0.0))
            return uvs;

        uvs.Reserve(static_cast<i32>(sampleCount));
        const f64* begin = cumulative.GetData();
        const f64* end = begin + cumulative.Num();
        for (u32 s = 0; s < sampleCount; ++s)
        {
            // Stratified over area: sample s lands somewhere in the s-th of
            // sampleCount equal slices of the total, so no region of the mesh
            // is over- or under-sampled by chance.
            const f64 slice = (static_cast<f64>(s) + static_cast<f64>(Unit(Hash(s * 3u + 0x9e3779b9u)))) /
                              static_cast<f64>(sampleCount);
            const f64 target = slice * total;
            const f64* hit = std::upper_bound(begin, end, target);
            const auto triangle = static_cast<i32>(std::min<std::ptrdiff_t>(hit - begin, cumulative.Num() - 1));
            const u32 base = firstIndex[triangle];

            // Uniform point in the triangle (the square-root warp).
            const f32 r1 = std::sqrt(Unit(Hash(s * 3u + 1u)));
            const f32 r2 = Unit(Hash(s * 3u + 2u));
            const glm::vec2& uv0 = vertices[indices[base + 0]].TexCoord;
            const glm::vec2& uv1 = vertices[indices[base + 1]].TexCoord;
            const glm::vec2& uv2 = vertices[indices[base + 2]].TexCoord;
            uvs.Add((1.0f - r1) * uv0 + r1 * (1.0f - r2) * uv1 + r1 * r2 * uv2);
        }
        return uvs;
    }
} // namespace OloEngine::FoliageAlphaCoverage

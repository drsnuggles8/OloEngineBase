#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomCoat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace OloEngine
{
    namespace
    {
        // Lower-cases ASCII only. A group name is an identifier out of a DCC,
        // and a locale-sensitive tolower() would make role inference depend on
        // the machine's locale — which is a determinism leak of exactly the kind
        // GroomCooker.h forbids, arriving through the back door.
        [[nodiscard]] std::string AsciiLower(std::string_view text)
        {
            std::string lowered(text);
            for (char& c : lowered)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return lowered;
        }

        [[nodiscard]] bool Contains(const std::string& haystack, std::string_view needle) noexcept
        {
            return haystack.find(needle) != std::string::npos;
        }

        // Replaces `value` with `fallback` when it is not finite or falls
        // outside [lo, hi], and names what happened.
        //
        // The finite test comes FIRST and is separate from the range test: a NaN
        // fails every comparison, so `value < lo || value > hi` is false for it
        // and a range-only guard passes NaN straight through.
        [[nodiscard]] bool Repair(f32& value, f32 lo, f32 hi, f32 fallback, const char* what, u32 groupIndex,
                                  std::vector<std::string>& outReasons)
        {
            if (!std::isfinite(value))
            {
                outReasons.push_back(std::format("group {}: {} is not finite; reset to {}", groupIndex, what, fallback));
                value = fallback;
                return false;
            }
            if (value < lo || value > hi)
            {
                outReasons.push_back(
                    std::format("group {}: {} is {}, outside [{}, {}]; reset to {}", groupIndex, what, value, lo, hi, fallback));
                value = fallback;
                return false;
            }
            return true;
        }
    } // namespace

    GroomCoatRole InferGroomCoatRole(std::string_view groupName) noexcept
    {
        // Ordered most specific first. "guard" before "under" matters for a
        // group named "underguard"; whiskers before everything because a group
        // named "muzzle_whiskers_long" is whiskers, not long hair.
        const std::string name = AsciiLower(groupName);

        if (Contains(name, "whisker") || Contains(name, "vibriss"))
        {
            return GroomCoatRole::Whisker;
        }
        if (Contains(name, "guard") || Contains(name, "topcoat") || Contains(name, "top_coat") ||
            Contains(name, "overcoat"))
        {
            return GroomCoatRole::GuardHair;
        }
        if (Contains(name, "undercoat") || Contains(name, "under_coat") || Contains(name, "underfur") ||
            Contains(name, "downy") || Contains(name, "fluff"))
        {
            return GroomCoatRole::Undercoat;
        }
        if (Contains(name, "mane") || Contains(name, "plume") || Contains(name, "feather") ||
            Contains(name, "longhair") || Contains(name, "long_hair") || Contains(name, "tail_hair"))
        {
            return GroomCoatRole::LongHair;
        }
        // NOT a guess dressed as an answer. A group called "body" or "flank"
        // says nothing about which layer it is, and assigning it a role would
        // put it under a slider the artist never asked for.
        return GroomCoatRole::Unassigned;
    }

    bool SanitizeGroomCoatGroupDesc(GroomCoatGroupDesc& desc, u32 groupIndex, std::vector<std::string>& outReasons)
    {
        // `&=`, NOT `&&=`: every field must be repaired, so the calls must all
        // run. Short-circuiting on the first bad value would leave the rest of
        // the description unrepaired and unreported, which is the shape of a
        // silent fallback.
        bool clean = true;
        clean &= Repair(desc.Density, GroomCoatLimits::MinDensity, GroomCoatLimits::MaxDensity, 1.0f, "density",
                        groupIndex, outReasons);
        clean &= Repair(desc.Length, GroomCoatLimits::MinLength, GroomCoatLimits::MaxLength, 1.0f, "length",
                        groupIndex, outReasons);
        clean &= Repair(desc.Width, GroomCoatLimits::MinWidth, GroomCoatLimits::MaxWidth, 1.0f, "width", groupIndex,
                        outReasons);
        clean &= Repair(desc.Clump, GroomCoatLimits::MinClump, GroomCoatLimits::MaxClump, 0.0f, "clump", groupIndex,
                        outReasons);
        clean &= Repair(desc.Tint.r, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tint.r", groupIndex,
                        outReasons);
        clean &= Repair(desc.Tint.g, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tint.g", groupIndex,
                        outReasons);
        clean &= Repair(desc.Tint.b, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tint.b", groupIndex,
                        outReasons);

        // ── Coat authoring v4 (#1533) ──
        // Each reset lands on the value that changes nothing: a white tip, no
        // curl, no wave, the entity's own stiffness.
        clean &= Repair(desc.TipTint.r, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tip tint.r",
                        groupIndex, outReasons);
        clean &= Repair(desc.TipTint.g, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tip tint.g",
                        groupIndex, outReasons);
        clean &= Repair(desc.TipTint.b, GroomCoatLimits::MinTint, GroomCoatLimits::MaxTint, 1.0f, "tip tint.b",
                        groupIndex, outReasons);
        clean &= Repair(desc.CurlRadius, GroomCoatLimits::MinCurlRadius, GroomCoatLimits::MaxCurlRadius, 0.0f,
                        "curl radius", groupIndex, outReasons);
        clean &= Repair(desc.CurlFrequency, GroomCoatLimits::MinCurlFrequency, GroomCoatLimits::MaxCurlFrequency, 0.0f,
                        "curl frequency", groupIndex, outReasons);
        clean &= Repair(desc.WaveAmplitude, GroomCoatLimits::MinWaveAmplitude, GroomCoatLimits::MaxWaveAmplitude, 0.0f,
                        "wave amplitude", groupIndex, outReasons);
        clean &= Repair(desc.WaveFrequency, GroomCoatLimits::MinWaveFrequency, GroomCoatLimits::MaxWaveFrequency, 0.0f,
                        "wave frequency", groupIndex, outReasons);
        clean &= Repair(desc.StiffnessScale, GroomCoatLimits::MinStiffnessScale, GroomCoatLimits::MaxStiffnessScale,
                        1.0f, "stiffness scale", groupIndex, outReasons);

        if (!IsValidGroomCoatRole(static_cast<i32>(desc.Role)))
        {
            outReasons.push_back(std::format("group {}: role {} is not a GroomCoatRole; reset to Unassigned",
                                             groupIndex, desc.Role));
            desc.Role = static_cast<u8>(GroomCoatRole::Unassigned);
            clean = false;
        }

        // The pads go to disk. Zeroing them here is what makes two cooks of the
        // same groom byte-identical even when the description came from a struct
        // some caller memcpy'd out of a larger buffer.
        desc.Pad0 = 0;
        desc.Pad1 = 0;
        desc.Pad2 = 0;
        return clean;
    }

    GroomCoatGroupDesc DefaultGroomCoatGroupDesc() noexcept
    {
        return GroomCoatGroupDesc{};
    }

    // ── GroomRegionMap ───────────────────────────────────────────────

    Ref<GroomRegionMap> GroomRegionMap::FromRGBA8(u32 width, u32 height, std::span<const u8> rgba)
    {
        if (width == 0u || height == 0u)
        {
            return nullptr;
        }
        const auto expected = static_cast<sizet>(width) * static_cast<sizet>(height) * 4u;
        if (rgba.size() != expected)
        {
            // Refused rather than padded. A short buffer padded with zeroes is a
            // map whose lower rows say "density zero", which is a bald animal
            // and no error anywhere.
            return nullptr;
        }

        return Adopt(width, height, std::vector<u8>(rgba.begin(), rgba.end()));
    }

    Ref<GroomRegionMap> GroomRegionMap::Adopt(u32 width, u32 height, std::vector<u8>&& rgba)
    {
        auto map = Ref<GroomRegionMap>::Create();
        map->m_Width = width;
        map->m_Height = height;
        map->m_Pixels = std::move(rgba);

        constexpr u64 offsetBasis = 14695981039346656037ull;
        constexpr u64 prime = 1099511628211ull;
        u64 hash = offsetBasis;
        for (const u8 byte : map->m_Pixels)
        {
            hash ^= static_cast<u64>(byte);
            hash *= prime;
        }
        // The dimensions are in the hash too: two maps whose pixel bytes happen
        // to be a reshape of each other are different maps.
        hash ^= (static_cast<u64>(width) << 32) | static_cast<u64>(height);
        hash *= prime;
        map->m_ContentHash = hash;
        return map;
    }

    Ref<GroomRegionMap> GroomRegionMap::FromRGB8(u32 width, u32 height, std::span<const u8> rgb)
    {
        if (width == 0u || height == 0u)
        {
            return nullptr;
        }
        const auto texels = static_cast<sizet>(width) * static_cast<sizet>(height);
        if (rgb.size() != texels * 3u)
        {
            return nullptr;
        }
        std::vector<u8> rgba(texels * 4u);
        for (sizet i = 0; i < texels; ++i)
        {
            rgba[(i * 4u) + 0u] = rgb[(i * 3u) + 0u];
            rgba[(i * 4u) + 1u] = rgb[(i * 3u) + 1u];
            rgba[(i * 4u) + 2u] = rgb[(i * 3u) + 2u];
            rgba[(i * 4u) + 3u] = 255u;
        }
        return Adopt(width, height, std::move(rgba));
    }

    glm::vec3 GroomRegionMap::Sample(glm::vec2 uv) const noexcept
    {
        if (m_Width == 0u || m_Height == 0u)
        {
            return glm::vec3(1.0f);
        }
        // Before any cast to an integer, not after: static_cast<i32> of a NaN is
        // undefined behaviour, and `u < 0.0f` is false for NaN so a sign check
        // does not catch it.
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
        {
            return glm::vec3(1.0f);
        }

        const f32 fx = std::clamp(uv.x, 0.0f, 1.0f) * static_cast<f32>(m_Width - 1u);
        const f32 fy = std::clamp(uv.y, 0.0f, 1.0f) * static_cast<f32>(m_Height - 1u);
        const auto x0 = static_cast<u32>(fx);
        const auto y0 = static_cast<u32>(fy);
        const u32 x1 = std::min(x0 + 1u, m_Width - 1u);
        const u32 y1 = std::min(y0 + 1u, m_Height - 1u);
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 ty = fy - static_cast<f32>(y0);

        const auto texel = [this](u32 x, u32 y)
        {
            const sizet offset = ((static_cast<sizet>(y) * m_Width) + x) * 4u;
            return glm::vec3(static_cast<f32>(m_Pixels[offset + 0]), static_cast<f32>(m_Pixels[offset + 1]),
                             static_cast<f32>(m_Pixels[offset + 2])) *
                   (1.0f / 255.0f);
        };

        const glm::vec3 top = glm::mix(texel(x0, y0), texel(x1, y0), tx);
        const glm::vec3 bottom = glm::mix(texel(x0, y1), texel(x1, y1), tx);
        return glm::mix(top, bottom, ty);
    }

    // ── Evaluation ───────────────────────────────────────────────────

    GroomCoatGroupDesc GroomCoatContext::GroupDesc(u16 groupId) const noexcept
    {
        const auto index = static_cast<sizet>(groupId);
        if (index >= Groups.size())
        {
            return GroomCoatGroupDesc{};
        }
        return Groups[index];
    }

    f32 GroomCoatContext::JitterScale(u16 groupId) const noexcept
    {
        const auto index = static_cast<sizet>(groupId);
        return index < JitterScales.size() ? JitterScales[index] : 1.0f;
    }

    GroomCoatStrandParams IdentityGroomCoatStrandParams() noexcept
    {
        return GroomCoatStrandParams{};
    }

    GroomCoatStrandParams EvaluateGroomCoatStrand(const GroomCoatContext& coat, u32 curveIndex, glm::vec2 rootUV,
                                                  u16 groupId) noexcept
    {
        GroomCoatStrandParams params;
        if (!coat.IsActive())
        {
            return params;
        }

        const GroomCoatSettings& settings = *coat.Settings;
        const GroomCoatGroupDesc desc = coat.GroupDesc(groupId);
        const GroomCoatRole role = desc.GetRole();

        if (!settings.IsRoleVisible(role))
        {
            params.Keep = false;
            return params;
        }

        // The role override. Only the two roles criterion 1 names have one; the
        // rest take identity, which is why this is a switch with a default
        // rather than a table lookup — the table would invite a fifth entry that
        // nothing sets.
        GroomCoatRoleOverride roleOverride{};
        switch (role)
        {
            case GroomCoatRole::Undercoat:
                roleOverride = settings.Undercoat;
                break;
            case GroomCoatRole::GuardHair:
                roleOverride = settings.Guard;
                break;
            case GroomCoatRole::Unassigned:
            case GroomCoatRole::Whisker:
            case GroomCoatRole::LongHair:
            case GroomCoatRole::Count:
                break;
        }

        // The regional map: R scales length, G scales density, B scales clump.
        // Sampled ONCE, here, and at the ROOT UV — so every consequence of it
        // travels with the strand through any deformation.
        glm::vec3 region(1.0f);
        if (settings.RegionMap)
        {
            region = settings.RegionMap->Sample(rootUV);
        }

        // ── Density: a hash draw, not a stride ───────────────────────
        //
        // A stride would decimate in COOKED ORDER, which after the cook is
        // spatial order within a group — so a density of 0.5 would remove every
        // other strand along the animal and leave visible stripes. A hash draw
        // on the curve index is spatially uncorrelated, deterministic, and
        // independent of how many other strands survived.
        const f32 density = std::clamp(desc.Density * roleOverride.Density * region.g, 0.0f, 1.0f);
        if (density < 1.0f)
        {
            // WHISKERS ARE NEVER DECIMATED. Twelve whiskers thinned to eight is
            // not a cheaper coat, it is a damaged animal — and it is the exact
            // failure that made this role exist. Their density is still
            // AUTHORABLE (a group authored at 0 is hidden), so the exemption is
            // from the runtime draw, not from the artist.
            const bool exempt = role == GroomCoatRole::Whisker;
            if (!exempt && GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::Density) >= density)
            {
                params.Keep = false;
                return params;
            }
        }

        // ── Length, width, clump ─────────────────────────────────────
        // Scaled on a card level: see GroomCoatContext::JitterScales.
        const f32 jitterScale = std::clamp(coat.JitterScale(groupId), 0.0f, 1.0f);
        const f32 lengthJitter = std::clamp(settings.LengthJitter, 0.0f, GroomCoatLimits::MaxJitter) * jitterScale;
        const f32 widthJitter = std::clamp(settings.WidthJitter, 0.0f, GroomCoatLimits::MaxJitter) * jitterScale;
        const f32 shadeJitter = std::clamp(settings.ShadeJitter, 0.0f, GroomCoatLimits::MaxJitter) * jitterScale;

        // (hash * 2 - 1) in [-1, 1), so the jitter is symmetric about the
        // authored value and the mean length of a group is unchanged by turning
        // jitter up. A one-sided jitter would make "more variation" also mean
        // "longer coat", which is two knobs pretending to be one.
        const f32 lengthNoise = (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::Length) * 2.0f) - 1.0f;
        const f32 widthNoise = (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::Width) * 2.0f) - 1.0f;
        const f32 shadeNoise = (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::Shade) * 2.0f) - 1.0f;

        params.Length = std::clamp(desc.Length * roleOverride.Length * region.r * (1.0f + (lengthJitter * lengthNoise)),
                                   GroomCoatLimits::MinLength, GroomCoatLimits::MaxLength);
        params.Width = std::clamp(desc.Width * roleOverride.Width * (1.0f + (widthJitter * widthNoise)),
                                  GroomCoatLimits::MinWidth, GroomCoatLimits::MaxWidth);
        params.Clump = std::clamp(desc.Clump * roleOverride.Clump * region.b, GroomCoatLimits::MinClump,
                                  GroomCoatLimits::MaxClump);

        // ── Tint, at the root and at the tip (#1533) ─────────────────
        //
        // The group's TipTint multiplies its Tint, and the map and the shade
        // jitter reach both ends identically, so the corner at t carries
        // map * shade * mix(Tint, Tint * TipTint, t). A group whose TipTint is
        // white (every group that authors none) evaluates the two ends to the
        // same bits -- a multiply by 1.0 is exact -- which PackGroomCoatTintAt
        // turns back into the one value v3 packed.
        glm::vec3 tint = desc.Tint;
        glm::vec3 tipTint = desc.Tint * desc.TipTint;
        if (settings.ColorMap)
        {
            const glm::vec3 sampled = settings.ColorMap->Sample(rootUV);
            tint *= sampled;
            tipTint *= sampled;
        }
        const f32 shade = 1.0f + (shadeJitter * shadeNoise);
        tint *= shade;
        tipTint *= shade;
        params.Tint = glm::clamp(tint, glm::vec3(0.0f), glm::vec3(GroomCoatLimits::MaxTint));
        params.TipTint = glm::clamp(tipTint, glm::vec3(0.0f), glm::vec3(GroomCoatLimits::MaxTint));

        // ── Curl and wave (#1533) ────────────────────────────────────
        //
        // The strand's own draws, on their own salts, keyed like everything
        // above on the curve index and the seed alone — so a curl is as
        // pose-invariant as a length. Symmetric jitter, so turning a group's
        // curl up never also makes it tighter or looser on average.
        //
        // THE AMPLITUDES CARRY THE CARD TIER'S JitterScale. A card stands for N
        // strands whose curls have independent phases, and N unit phasors of
        // random phase average to a magnitude of 1/sqrt(N): the mean of the
        // helices a card replaces is a helix that much narrower, the same factor
        // the length jitter above already carries (#1428). The frequency and the
        // phases are the strand's own draws and are not scaled.
        if (desc.CurlRadius > 0.0f && desc.CurlFrequency > 0.0f)
        {
            const f32 radiusNoise = (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::CurlRadius) * 2.0f) - 1.0f;
            const f32 frequencyNoise =
                (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::CurlFrequency) * 2.0f) - 1.0f;
            params.CurlRadius = desc.CurlRadius * (1.0f + (GroomCoatCurl::AmplitudeJitter * radiusNoise)) * jitterScale;
            params.CurlFrequency = desc.CurlFrequency * (1.0f + (GroomCoatCurl::FrequencyJitter * frequencyNoise));
            params.CurlPhase = GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::CurlPhase);
        }
        if (desc.WaveAmplitude > 0.0f && desc.WaveFrequency > 0.0f)
        {
            const f32 amplitudeNoise =
                (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::WaveAmplitude) * 2.0f) - 1.0f;
            const f32 frequencyNoise =
                (GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::WaveFrequency) * 2.0f) - 1.0f;
            params.WaveAmplitude =
                desc.WaveAmplitude * (1.0f + (GroomCoatCurl::AmplitudeJitter * amplitudeNoise)) * jitterScale;
            params.WaveFrequency = desc.WaveFrequency * (1.0f + (GroomCoatCurl::FrequencyJitter * frequencyNoise));
            params.WavePhase = GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::WavePhase);
        }
        if (params.CurlsOrWaves())
        {
            // Where around the root tangent the frame's normal starts: the
            // plane a wave oscillates in. A per-strand draw, or every strand of
            // a group would wave in one plane like a combed ribbon.
            params.CurlAzimuth = GroomCoatHash01(curveIndex, settings.Seed ^ GroomCoatSalt::CurlAzimuth);
        }
        return params;
    }

    u64 GroomCoatDigest(const GroomCoatSettings& settings) noexcept
    {
        constexpr u64 offsetBasis = 14695981039346656037ull;
        constexpr u64 prime = 1099511628211ull;
        u64 hash = offsetBasis;
        const auto mix = [&hash](u64 value)
        {
            hash ^= value;
            hash *= prime;
        };
        // FLOATS GO IN BY THEIR BIT PATTERN, never by a cast to an integer: two
        // lengths that differ in the last mantissa bit produce different
        // geometry, and rounding them together would serve one coat's strands
        // for the other's settings.
        const auto mixFloat = [&mix](f32 value)
        {
            mix(static_cast<u64>(std::bit_cast<u32>(value)));
        };
        const auto mixOverride = [&mixFloat](const GroomCoatRoleOverride& o)
        {
            mixFloat(o.Density);
            mixFloat(o.Length);
            mixFloat(o.Width);
            mixFloat(o.Clump);
        };

        mix(settings.Enabled ? 1ull : 0ull);
        if (!settings.Enabled)
        {
            // A disabled coat is skipped entirely by the build, so every other
            // field is irrelevant to the geometry — and folding them in anyway
            // would rebuild the mesh when an artist moved a slider on a coat
            // that is switched off.
            return hash;
        }
        mix(static_cast<u64>(settings.RoleVisibilityMask));
        mixOverride(settings.Undercoat);
        mixOverride(settings.Guard);
        mixFloat(settings.LengthJitter);
        mixFloat(settings.WidthJitter);
        mixFloat(settings.ShadeJitter);
        mixFloat(settings.ClumpCellSize);
        mix(static_cast<u64>(settings.Seed));
        mix(settings.RegionMap ? settings.RegionMap->GetContentHash() : 0ull);
        mix(settings.ColorMap ? settings.ColorMap->GetContentHash() : 0ull);
        return hash;
    }

    u64 GroomCoatClumpCell(glm::vec2 rootUV, f32 cellSize) noexcept
    {
        // Every guard before the cast, for the reason Sample() gives.
        const f32 size = std::isfinite(cellSize)
                             ? std::clamp(cellSize, GroomCoatLimits::MinClumpCellSize, GroomCoatLimits::MaxClumpCellSize)
                             : GroomCoatLimits::MaxClumpCellSize;
        const f32 u = std::isfinite(rootUV.x) ? std::clamp(rootUV.x, -16.0f, 16.0f) : 0.0f;
        const f32 v = std::isfinite(rootUV.y) ? std::clamp(rootUV.y, -16.0f, 16.0f) : 0.0f;

        // std::floor, not a truncating cast: a UV can legitimately be slightly
        // negative on a chart with a border, and truncation folds -0.3 and +0.3
        // into the same cell while 0.3 and 1.3 stay apart.
        const auto cellX = static_cast<i32>(std::floor(u / size));
        const auto cellY = static_cast<i32>(std::floor(v / size));
        return (static_cast<u64>(static_cast<u32>(cellX)) << 32) | static_cast<u64>(static_cast<u32>(cellY));
    }

    glm::vec3 ApplyGroomCoatShape(const glm::vec3& root, const glm::vec3& restPoint, f32 t, f32 lengthScale, f32 clump,
                                  const glm::vec3& clumpGrowth) noexcept
    {
        // Length first: scale the offset from the root, so the root is a fixed
        // point of the whole transformation and a shortened strand still starts
        // on the skin.
        const glm::vec3 scaled = root + ((restPoint - root) * lengthScale);
        if (clump <= 0.0f)
        {
            return scaled;
        }

        // The clump target for this parameter: my own root plus my clump's mean
        // growth vector, walked to t. Not the clump's mean POSITION — that would
        // drag roots sideways and detach the coat.
        const glm::vec3 target = root + (clumpGrowth * t);
        // t*t, not t: a linear pull bends the strand from the root and the coat
        // reads as combed. The quadratic leaves the base where the cook put it
        // and gathers only the outer half, which is what a tuft looks like.
        const f32 weight = std::clamp(clump, 0.0f, 1.0f) * t * t;
        return glm::mix(scaled, target, weight);
    }

    // ── Curl and wave (#1533) ───────────────────────────────────────

    namespace
    {
        constexpr f32 kTwoPi = 6.28318530717958647692f;

        // A chord or a tangent whose squared length is below this has no
        // direction. Squared lengths of real strand segments are ~1e-8 and
        // above (a tenth of a millimetre); this is "coincident", not "short".
        constexpr f32 kCurlDegenerate2 = 1.0e-20f;

        // Duff, Burgess, Christensen, Hery, Kensler, Liani and Villemin 2017,
        // "Building an Orthonormal Basis, Revisited": two unit vectors
        // perpendicular to unit `n` and to each other, with no branch on a
        // near-parallel reference axis. The basis jumps where n.z changes sign,
        // which does not matter here: the frame's starting azimuth is a
        // per-strand hash draw anyway.
        void OrthonormalBasis(const glm::vec3& n, glm::vec3& outB1, glm::vec3& outB2) noexcept
        {
            const f32 sign = std::copysign(1.0f, n.z);
            const f32 a = -1.0f / (sign + n.z);
            const f32 b = n.x * n.y * a;
            outB1 = glm::vec3(1.0f + (sign * n.x * n.x * a), sign * b, -sign * n.x);
            outB2 = glm::vec3(b, sign + (n.y * n.y * a), -n.y);
        }

        // A phase in TURNS, as an angle in [0, 2 pi). The turn count is carried
        // in double and wrapped BEFORE it narrows: at 400 turns a metre a long
        // strand reaches hundreds of turns, where an f32 holds the fraction to a
        // few 1e-5 of a turn and the error grows with every point along it.
        [[nodiscard]] f32 AngleOfTurns(f64 turns) noexcept
        {
            return static_cast<f32>(turns - std::floor(turns)) * kTwoPi;
        }

        [[nodiscard]] f32 CurlEnvelope(f32 t) noexcept
        {
            const f32 x = std::clamp(t / GroomCoatCurl::EnvelopeEnd, 0.0f, 1.0f);
            return x * x * (3.0f - (2.0f * x));
        }
    } // namespace

    void ApplyGroomCoatCurl(std::span<glm::vec3> points, const GroomCoatStrandParams& params) noexcept
    {
        const sizet count = points.size();
        if (count < 2u || !params.CurlsOrWaves())
        {
            return;
        }
        // Every parameter before any of them reaches a sine: a NaN phase would
        // otherwise put a NaN into every point but the root.
        if (!std::isfinite(params.CurlRadius) || !std::isfinite(params.CurlFrequency) ||
            !std::isfinite(params.CurlPhase) || !std::isfinite(params.CurlAzimuth) ||
            !std::isfinite(params.WaveAmplitude) || !std::isfinite(params.WaveFrequency) ||
            !std::isfinite(params.WavePhase))
        {
            return;
        }
        const bool curl = params.CurlRadius > 0.0f && params.CurlFrequency > 0.0f;
        const bool wave = params.WaveAmplitude > 0.0f && params.WaveFrequency > 0.0f;

        // The root tangent: toward the first point that is not the root.
        glm::vec3 tangent(0.0f);
        bool directed = false;
        for (sizet i = 1; i < count && !directed; ++i)
        {
            const glm::vec3 chord = points[i] - points[0];
            const f32 chord2 = glm::dot(chord, chord);
            if (chord2 > kCurlDegenerate2)
            {
                tangent = chord / std::sqrt(chord2);
                directed = true;
            }
        }
        if (!directed)
        {
            return; // every point sits on the root: nothing to curl about
        }

        glm::vec3 b1(0.0f);
        glm::vec3 b2(0.0f);
        OrthonormalBasis(tangent, b1, b2);
        const f32 azimuth = kTwoPi * params.CurlAzimuth;
        glm::vec3 normal = (std::cos(azimuth) * b1) + (std::sin(azimuth) * b2);

        const f32 invSpan = 1.0f / static_cast<f32>(count - 1u);
        // The ORIGINAL position of the point before i. Points are overwritten as
        // the walk passes them, and both the arc length and the frame must be
        // measured on the shaped strand, not on the curled one.
        glm::vec3 previous = points[0];
        glm::vec3 previousTangent = tangent;
        f64 arc = 0.0; // in double for AngleOfTurns' reason: it only ever grows
        for (sizet i = 1; i < count; ++i)
        {
            const glm::vec3 current = points[i];
            const glm::vec3 chord = current - previous;
            const f32 chord2 = glm::dot(chord, chord);
            arc += std::sqrt(static_cast<f64>(chord2));

            // The tangent AT point i: across it, from the point before to the
            // point after (neither of which is curled yet — the one after has not
            // been reached), and one-sided at the tip.
            const glm::vec3 across = (i + 1u < count) ? (points[i + 1u] - previous) : chord;
            const f32 across2 = glm::dot(across, across);
            const glm::vec3 pointTangent = across2 > kCurlDegenerate2 ? across / std::sqrt(across2) : previousTangent;

            // Double reflection: reflect the frame in the plane bisecting the
            // chord, then in the plane that takes the reflected tangent onto
            // this point's tangent. The composition is the rotation-minimising
            // transport, exact for a straight run and second-order along a bend.
            glm::vec3 reflectedNormal = normal;
            glm::vec3 reflectedTangent = previousTangent;
            if (chord2 > kCurlDegenerate2)
            {
                reflectedNormal -= ((2.0f / chord2) * glm::dot(chord, normal)) * chord;
                reflectedTangent -= ((2.0f / chord2) * glm::dot(chord, previousTangent)) * chord;
            }
            const glm::vec3 second = pointTangent - reflectedTangent;
            if (const f32 second2 = glm::dot(second, second); second2 > kCurlDegenerate2)
            {
                reflectedNormal -= ((2.0f / second2) * glm::dot(second, reflectedNormal)) * second;
            }
            // Re-orthonormalised against the new tangent: the drift is rounding,
            // not geometry, and it compounds over a long strand.
            const glm::vec3 projected = reflectedNormal - (glm::dot(reflectedNormal, pointTangent) * pointTangent);
            if (const f32 projected2 = glm::dot(projected, projected); projected2 > kCurlDegenerate2)
            {
                normal = projected / std::sqrt(projected2);
            }
            else
            {
                // Only a strand that folds straight back on itself gets here;
                // restart the frame rather than divide by nothing.
                OrthonormalBasis(pointTangent, b1, b2);
                normal = b1;
            }
            const glm::vec3 binormal = glm::cross(pointTangent, normal);

            const f32 envelope = CurlEnvelope(static_cast<f32>(i) * invSpan);
            if (envelope > 0.0f)
            {
                glm::vec3 offset(0.0f);
                if (curl)
                {
                    const f32 angle = AngleOfTurns((static_cast<f64>(params.CurlFrequency) * arc) +
                                                   static_cast<f64>(params.CurlPhase));
                    offset += params.CurlRadius * ((std::cos(angle) * normal) + (std::sin(angle) * binormal));
                }
                if (wave)
                {
                    const f32 angle = AngleOfTurns((static_cast<f64>(params.WaveFrequency) * arc) +
                                                   static_cast<f64>(params.WavePhase));
                    offset += (params.WaveAmplitude * std::sin(angle)) * normal;
                }
                points[i] = current + (envelope * offset);
            }

            previous = current;
            previousTangent = pointTangent;
        }
    }
} // namespace OloEngine

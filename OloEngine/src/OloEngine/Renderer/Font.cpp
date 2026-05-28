#include "OloEnginePCH.h"
#include "Font.h"
#include "SlugData.h"
#include "SlugFontProcessor.h"

#include "OloEngine/Core/UTF8.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_image/stb_truetype.h>

#include <fstream>

namespace OloEngine
{
    Font::Font(const std::filesystem::path& filepath)
        : m_Data(CreateScope<SlugFontData>())
    {
        // Default codepoint coverage: Latin-1 — preserves the original
        // behaviour for fonts loaded through the no-range constructor.
        LoadFromFile(filepath, { FontCodepointRanges::Latin1 });
    }

    Font::Font(const std::filesystem::path& filepath, const std::vector<FontCodepointRange>& ranges)
        : m_Data(CreateScope<SlugFontData>())
    {
        LoadFromFile(filepath, ranges);
    }

    void Font::LoadFromFile(const std::filesystem::path& filepath, const std::vector<FontCodepointRange>& ranges)
    {
        m_Name = filepath.filename().stem().string();
        m_Path = filepath.string();

        // Read TTF file into memory
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("Failed to open font file: {}", m_Path);
            return;
        }

        const auto fileSize = file.tellg();
        if (fileSize <= 0)
        {
            OLO_CORE_ERROR("Failed to determine font file size (tellg={}) for: {}", static_cast<std::streamoff>(fileSize), m_Path);
            return;
        }

        if (constexpr std::streamoff kMaxFontFileSize = 64 * 1024 * 1024 /* 64 MB sanity cap */; fileSize > kMaxFontFileSize)
        {
            OLO_CORE_ERROR("Font file too large ({} bytes, max {}) for: {}", static_cast<std::streamoff>(fileSize), kMaxFontFileSize, m_Path);
            return;
        }

        file.seekg(0, std::ios::beg);
        if (!file.good())
        {
            OLO_CORE_ERROR("Failed to seek to start of font file: {}", m_Path);
            return;
        }

        std::vector<u8> fontBuffer(static_cast<sizet>(fileSize));
        file.read(reinterpret_cast<char*>(fontBuffer.data()), fileSize);
        if (file.fail())
        {
            OLO_CORE_ERROR("Failed to read font file: {}", m_Path);
            return;
        }

        // Initialize stb_truetype
        ::stbtt_fontinfo fontInfo{};
        if (::stbtt_InitFont(&fontInfo, fontBuffer.data(), ::stbtt_GetFontOffsetForIndex(fontBuffer.data(), 0)) == 0)
        {
            OLO_CORE_ERROR("stb_truetype failed to parse font: {}", m_Path);
            return;
        }

        // Extract global metrics
        int ascent{};
        int descent{};
        int lineGap{};
        ::stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);

        // unitsPerEm scales raw font units → normalized em-space
        f32 unitsPerEm = static_cast<f32>(ascent - descent);
        if (!std::isfinite(unitsPerEm) || std::abs(unitsPerEm) < 1e-6f)
        {
            OLO_CORE_ERROR("Font '{}' has invalid metrics (ascent={}, descent={}) — using fallback unitsPerEm=1.0", m_Path, ascent, descent);
            unitsPerEm = 1.0f;
        }
        const f32 emScale = 1.0f / unitsPerEm;
        m_Data->Metrics.AscenderY = static_cast<f32>(ascent) * emScale;
        m_Data->Metrics.DescenderY = static_cast<f32>(descent) * emScale;
        m_Data->Metrics.LineHeight = static_cast<f32>(ascent - descent + lineGap) * emScale;
        m_Data->Metrics.UnitsPerEm = unitsPerEm;

        // Walk every requested codepoint range and pull glyph metrics for
        // each glyph stb_truetype recognises. Ranges that overlap (e.g. a
        // caller supplying both Latin1 and ArabicBasic) are de-duplicated
        // by `Glyphs[codepoint] = glyph` overwriting in place — cheap.
        int glyphCount = 0;
        for (const auto& range : ranges)
        {
            for (u32 codepoint = range.First; codepoint <= range.Last; ++codepoint)
            {
                const int glyphIndex = ::stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(codepoint));
                if (glyphIndex == 0 && codepoint != ' ')
                    continue; // glyph not present in font

                int advanceWidth{};
                int leftSideBearing{};
                ::stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);

                int x0{}, y0{}, x1{}, y1{};
                ::stbtt_GetGlyphBox(&fontInfo, glyphIndex, &x0, &y0, &x1, &y1);

                SlugGlyphData glyph;
                glyph.AdvanceWidth = static_cast<f32>(advanceWidth) * emScale;
                glyph.PlaneBoundsLeft = static_cast<f32>(x0) * emScale;
                glyph.PlaneBoundsBottom = static_cast<f32>(y0) * emScale;
                glyph.PlaneBoundsRight = static_cast<f32>(x1) * emScale;
                glyph.PlaneBoundsTop = static_cast<f32>(y1) * emScale;

                m_Data->Glyphs[codepoint] = glyph;
                ++glyphCount;
            }
        }

        // Load kerning pairs — use codepoint-based lookup to match GetAdvance().
        // stbtt_GetKerningTable returns glyph indices, so we build a reverse
        // mapping (glyphIndex → codepoint) for correct key construction.
        if (const int kernTableLength = ::stbtt_GetKerningTableLength(&fontInfo); kernTableLength > 0)
        {
            // Build glyphIndex → codepoint reverse map for the loaded charset.
            // Multiple codepoints can share the same glyph index (aliases),
            // so store all codepoints per glyph to emit kerning for every pair.
            std::unordered_map<int, std::vector<u32>> glyphIndexToCodepoints;
            for (const auto& range : ranges)
            {
                for (u32 cp = range.First; cp <= range.Last; ++cp)
                {
                    const int gi = ::stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(cp));
                    if (gi != 0 || cp == ' ')
                        glyphIndexToCodepoints[gi].push_back(cp);
                }
            }

            std::vector<::stbtt_kerningentry> kernTable(static_cast<sizet>(kernTableLength));
            ::stbtt_GetKerningTable(&fontInfo, kernTable.data(), kernTableLength);
            for (const auto& entry : kernTable)
            {
                if (entry.advance == 0)
                {
                    continue;
                }
                auto it1 = glyphIndexToCodepoints.find(entry.glyph1);
                auto it2 = glyphIndexToCodepoints.find(entry.glyph2);
                if (it1 != glyphIndexToCodepoints.end() && it2 != glyphIndexToCodepoints.end())
                {
                    const f32 kernValue = static_cast<f32>(entry.advance) * emScale;
                    for (u32 cp1 : it1->second)
                    {
                        for (u32 cp2 : it2->second)
                        {
                            const u64 key = (static_cast<u64>(cp1) << 32) | cp2;
                            m_Data->KerningPairs[key] = kernValue;
                        }
                    }
                }
            }
        }

        OLO_CORE_INFO("Loaded {} glyphs from font '{}' via stb_truetype", glyphCount, m_Name);

        // Generate Slug curve + band textures.
        SlugFontProcessor::Process(fontInfo, emScale, *m_Data);

        m_IsLoaded = true;
    }

    Font::~Font() = default;

    Ref<Font> Font::GetDefault()
    {
        static Ref<Font> DefaultFont;
        if (DefaultFont && DefaultFont->IsLoaded())
        {
            return DefaultFont;
        }

        // Search a small list of well-known locations. Production code runs
        // from `OloEditor/` (per CLAUDE.md), so the CWD-relative path wins
        // there. Test binaries and headless tools run from the repo root,
        // where the editor-relative variant is the one that exists. The
        // first path that produces a loaded Font is cached.
        static constexpr const char* kCandidates[] = {
            "assets/fonts/opensans/OpenSans-Regular.ttf",
            "OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
            "../OloEditor/assets/fonts/opensans/OpenSans-Regular.ttf",
        };
        for (const char* candidate : kCandidates)
        {
            if (std::error_code ec; !std::filesystem::exists(candidate, ec) || ec)
            {
                continue;
            }
            auto candidateFont = Font::Create(candidate);
            if (candidateFont && candidateFont->IsLoaded())
            {
                DefaultFont = candidateFont;
                return DefaultFont;
            }
        }

        // Nothing on disk loaded. Cache whatever the last attempt produced
        // (an unloaded Font sentinel) so accessors can null-check via
        // IsLoaded() without infinite re-load attempts.
        if (!DefaultFont)
        {
            DefaultFont = Font::Create(kCandidates[0]);
        }
        return DefaultFont;
    }

    Ref<Font> Font::Create(const std::filesystem::path& font)
    {
        OLO_PROFILE_FUNCTION();

        auto canonical = std::filesystem::weakly_canonical(font).string();
        static std::unordered_map<std::string, WeakRef<Font>> s_FontCache;
        static FMutex s_FontCacheMutex;

        {
            TUniqueLock<FMutex> lock(s_FontCacheMutex);
            auto it = s_FontCache.find(canonical);
            if (it != s_FontCache.end())
            {
                if (auto cached = it->second.Lock())
                    return cached;
                s_FontCache.erase(it);
            }
        }

        auto newFont = Ref<Font>::Create(canonical);

        if (newFont->IsLoaded())
        {
            // Double-checked: another thread may have loaded and cached the same
            // font while we were loading outside the lock. Re-check and reuse its
            // entry so concurrent callers share a single Font instance.
            TUniqueLock<FMutex> lock(s_FontCacheMutex);
            auto it = s_FontCache.find(canonical);
            if (it != s_FontCache.end())
            {
                if (auto cached = it->second.Lock())
                    return cached;
                s_FontCache.erase(it);
            }
            s_FontCache[canonical] = newFont;
        }
        return newFont;
    }

    Ref<Font> Font::Create(const std::filesystem::path& font, const std::vector<FontCodepointRange>& ranges)
    {
        // No cache for ranged variants — the cache key would need to fold
        // the range list in, and the typical use (one ranged load per
        // game-locale at startup) doesn't benefit from sharing.
        return Ref<Font>::Create(std::filesystem::weakly_canonical(font).string(), ranges);
    }

    Font::GlyphLookup Font::FindGlyphWithFallback(u32 codepoint) const
    {
        if (m_Data)
        {
            if (const auto* g = m_Data->GetGlyph(codepoint))
                return { this, g };
        }
        for (const auto& fallback : m_FallbackFonts)
        {
            if (!fallback)
                continue;
            // Recursive fallback walk lets chains nest (Latin → CJK → Emoji).
            // Cycles aren't expected — the typical chain is short and authored
            // by the locale config — so we don't track visited fonts.
            auto inner = fallback->FindGlyphWithFallback(codepoint);
            if (inner.Glyph)
                return inner;
        }
        return {};
    }

    f32 Font::MeasureLine(std::string_view line, f32 fsScale, f32 kerning) const
    {
        if (!m_IsLoaded || !m_Data)
            return 0.0f;
        if (line.empty())
            return 0.0f;

        OLO_PROFILE_FUNCTION();

        // Mirrors the advancement pattern in Renderer2D::DrawString: UTF-8
        // codepoint iteration, fallback resolution, primary-font kerning.
        // Space-glyph advance is captured up front so empty fallback rows
        // don't pay a per-iteration map lookup.
        const auto* spaceGlyph = m_Data->GetGlyph(' ');
        const f32 spaceAdvance = spaceGlyph ? spaceGlyph->AdvanceWidth : 0.25f;

        const auto peekNextCodepoint = [&line](sizet pos) -> u32
        {
            if (pos >= line.size())
                return 0u;
            u32 cp = 0;
            sizet adv = 0;
            UTF8::DecodeCodepoint(line, pos, cp, adv);
            return cp;
        };

        // Kerning across fonts isn't meaningfully defined — only honour
        // the kerning table when both `cp` and `next` resolve against the
        // primary font.
        const auto resolveAdvance = [this, &spaceAdvance](u32 cp, u32 next, const GlyphLookup& lookup) -> f32
        {
            if (!lookup.Glyph)
                return spaceAdvance;
            if (lookup.SourceFont == this && next != 0u)
            {
                if (auto nextLookup = FindGlyphWithFallback(next); nextLookup.SourceFont == this)
                    return m_Data->GetAdvance(cp, next);
            }
            return lookup.Glyph->AdvanceWidth;
        };

        double x = 0.0;
        sizet i = 0;
        while (i < line.size())
        {
            u32 character = 0;
            sizet adv = 0;
            UTF8::DecodeCodepoint(line, i, character, adv);
            const sizet next = i + adv;

            if (character == '\r')
            {
                i = next;
                continue;
            }

            if (character == ' ')
            {
                f32 advance = spaceAdvance;
                if (next < line.size())
                    advance = m_Data->GetAdvance(character, peekNextCodepoint(next));
                x += static_cast<double>(fsScale) * advance + kerning;
                i = next;
                continue;
            }

            if (character == '\t')
            {
                x += 4.0 * (static_cast<double>(fsScale) * spaceAdvance + kerning);
                i = next;
                continue;
            }

            auto lookup = FindGlyphWithFallback(character);
            if (!lookup.Glyph)
                lookup = FindGlyphWithFallback('?');
            if (!lookup.Glyph)
            {
                i = next;
                continue;
            }

            const u32 nextCp = (next < line.size()) ? peekNextCodepoint(next) : 0u;
            const f32 advance = resolveAdvance(character, nextCp, lookup);
            x += static_cast<double>(fsScale) * advance + kerning;
            i = next;
        }

        return static_cast<f32>(x);
    }
} // namespace OloEngine

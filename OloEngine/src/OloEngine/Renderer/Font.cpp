#include "OloEnginePCH.h"
#include "Font.h"
#include "SlugData.h"
#include "SlugFontProcessor.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_image/stb_truetype.h>

#include <fstream>

namespace OloEngine
{
    Font::Font(const std::filesystem::path& filepath)
        : m_Data(CreateScope<SlugFontData>())
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

        constexpr std::streamoff kMaxFontFileSize = 64 * 1024 * 1024; // 64 MB sanity cap
        if (fileSize > kMaxFontFileSize)
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
        if (!file.read(reinterpret_cast<char*>(fontBuffer.data()), fileSize))
        {
            OLO_CORE_ERROR("Failed to read font file: {}", m_Path);
            return;
        }

        // Initialize stb_truetype
        stbtt_fontinfo fontInfo{};
        if (!stbtt_InitFont(&fontInfo, fontBuffer.data(), stbtt_GetFontOffsetForIndex(fontBuffer.data(), 0)))
        {
            OLO_CORE_ERROR("stb_truetype failed to parse font: {}", m_Path);
            return;
        }

        // Extract global metrics
        int ascent{};
        int descent{};
        int lineGap{};
        stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);

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

        // Load glyph metrics for ASCII printable range (matches previous MSDF charset)
        constexpr u32 charsetBegin = 0x0020;
        constexpr u32 charsetEnd = 0x00FF;
        int glyphCount = 0;

        for (u32 codepoint = charsetBegin; codepoint <= charsetEnd; ++codepoint)
        {
            const int glyphIndex = stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(codepoint));
            if (glyphIndex == 0 && codepoint != ' ')
            {
                continue; // glyph not present in font
            }

            int advanceWidth{};
            int leftSideBearing{};
            stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advanceWidth, &leftSideBearing);

            int x0{};
            int y0{};
            int x1{};
            int y1{};
            stbtt_GetGlyphBox(&fontInfo, glyphIndex, &x0, &y0, &x1, &y1);

            SlugGlyphData glyph;
            glyph.AdvanceWidth = static_cast<f32>(advanceWidth) * emScale;
            glyph.PlaneBoundsLeft = static_cast<f32>(x0) * emScale;
            glyph.PlaneBoundsBottom = static_cast<f32>(y0) * emScale;
            glyph.PlaneBoundsRight = static_cast<f32>(x1) * emScale;
            glyph.PlaneBoundsTop = static_cast<f32>(y1) * emScale;

            m_Data->Glyphs[codepoint] = glyph;
            ++glyphCount;
        }

        // Load kerning pairs — use codepoint-based lookup to match GetAdvance().
        // stbtt_GetKerningTable returns glyph indices, so we build a reverse
        // mapping (glyphIndex → codepoint) for correct key construction.
        const int kernTableLength = stbtt_GetKerningTableLength(&fontInfo);
        if (kernTableLength > 0)
        {
            // Build glyphIndex → codepoint reverse map for the loaded charset.
            std::unordered_map<int, u32> glyphIndexToCodepoint;
            for (u32 cp = charsetBegin; cp <= charsetEnd; ++cp)
            {
                const int gi = stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(cp));
                if (gi != 0 || cp == ' ')
                {
                    glyphIndexToCodepoint[gi] = cp;
                }
            }

            std::vector<stbtt_kerningentry> kernTable(static_cast<sizet>(kernTableLength));
            stbtt_GetKerningTable(&fontInfo, kernTable.data(), kernTableLength);
            for (const auto& entry : kernTable)
            {
                if (entry.advance == 0)
                {
                    continue;
                }
                auto it1 = glyphIndexToCodepoint.find(entry.glyph1);
                auto it2 = glyphIndexToCodepoint.find(entry.glyph2);
                if (it1 != glyphIndexToCodepoint.end() && it2 != glyphIndexToCodepoint.end())
                {
                    const u64 key = (static_cast<u64>(it1->second) << 32) | it2->second;
                    m_Data->KerningPairs[key] = static_cast<f32>(entry.advance) * emScale;
                }
            }
        }

        OLO_CORE_INFO("Loaded {} glyphs from font '{}' via stb_truetype", glyphCount, m_Name);

        // Generate Slug curve + band textures.
        SlugFontProcessor::Process(fontInfo, emScale, *m_Data);
    }

    Font::~Font()
    {
        // m_Data is automatically cleaned up by Scope<SlugFontData> (std::unique_ptr)
    }

    Ref<Font> Font::GetDefault()
    {
        static Ref<Font> DefaultFont;
        if (!DefaultFont)
        {
            DefaultFont = Font::Create("assets/fonts/opensans/OpenSans-Regular.ttf");
        }

        return DefaultFont;
    }

    Ref<Font> Font::Create(const std::filesystem::path& font)
    {
        auto canonical = std::filesystem::weakly_canonical(font).string();
        static std::unordered_map<std::string, WeakRef<Font>> s_FontCache;
        static std::mutex s_FontCacheMutex;

        {
            std::lock_guard lock(s_FontCacheMutex);
            auto it = s_FontCache.find(canonical);
            if (it != s_FontCache.end())
            {
                if (auto cached = it->second.Lock())
                    return cached;
                s_FontCache.erase(it);
            }
        }

        auto newFont = Ref<Font>::Create(canonical);

        {
            std::lock_guard lock(s_FontCacheMutex);
            s_FontCache[canonical] = newFont;
        }
        return newFont;
    }
} // namespace OloEngine

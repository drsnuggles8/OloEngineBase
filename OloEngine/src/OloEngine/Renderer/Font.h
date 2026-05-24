#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/SlugData.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // Inclusive codepoint range loaded into the font's glyph map. Most fonts
    // only ship glyphs for a subset of Unicode; loading every possible block
    // would be wasteful, so callers declare what they actually render.
    struct FontCodepointRange
    {
        u32 First = 0u;
        u32 Last = 0u;

        constexpr FontCodepointRange() = default;
        constexpr FontCodepointRange(u32 f, u32 l) : First(f), Last(l) {}
    };

    namespace FontCodepointRanges
    {
        // Curated Unicode blocks for common locales. Add more as needed —
        // each block is small and selective; loading "everything" is a
        // memory + curve-texture cost we don't pay unless asked.
        inline constexpr FontCodepointRange Latin1{ 0x0020, 0x00FF }; // matches the legacy default
        inline constexpr FontCodepointRange LatinExtA{ 0x0100, 0x017F };
        inline constexpr FontCodepointRange LatinExtB{ 0x0180, 0x024F };
        inline constexpr FontCodepointRange Cyrillic{ 0x0400, 0x04FF };
        inline constexpr FontCodepointRange Greek{ 0x0370, 0x03FF };
        inline constexpr FontCodepointRange Hebrew{ 0x0590, 0x05FF };
        inline constexpr FontCodepointRange ArabicBasic{ 0x0600, 0x06FF };
        inline constexpr FontCodepointRange Hiragana{ 0x3040, 0x309F };
        inline constexpr FontCodepointRange Katakana{ 0x30A0, 0x30FF };
        inline constexpr FontCodepointRange CJKUnifiedHan{ 0x4E00, 0x9FBF };
    } // namespace FontCodepointRanges

    class Font : public RendererResource
    {
      public:
        explicit Font(const std::filesystem::path& font);
        Font(const std::filesystem::path& font, const std::vector<FontCodepointRange>& ranges);
        ~Font() override;

        [[nodiscard("Store this!")]] const SlugFontData* GetSlugData() const
        {
            return m_IsLoaded ? m_Data.get() : nullptr;
        }

        // Legacy accessor kept temporarily so callers compile during migration.
        // Returns nullptr — callers that used MSDFData should migrate to GetSlugData().
        [[deprecated("Use GetSlugData() instead")]] [[nodiscard("Store this!")]] const void* GetMSDFData() const
        {
            return nullptr;
        }

        Ref<Texture2D> GetCurveTexture() const
        {
            return (m_IsLoaded && m_Data) ? m_Data->CurveTexture : nullptr;
        }

        Ref<Texture2D> GetBandTexture() const
        {
            return (m_IsLoaded && m_Data) ? m_Data->BandTexture : nullptr;
        }

        // Legacy accessor — returns nullptr. Use GetCurveTexture() / GetBandTexture().
        [[deprecated("Use GetCurveTexture() / GetBandTexture() instead")]]
        Ref<Texture2D> GetAtlasTexture() const
        {
            return nullptr;
        }

        const std::string& GetName() const
        {
            return m_Name;
        }
        const std::string& GetPath() const
        {
            return m_Path;
        }

        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Font;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        static Ref<Font> GetDefault();
        static Ref<Font> Create(const std::filesystem::path& font);
        static Ref<Font> Create(const std::filesystem::path& font, const std::vector<FontCodepointRange>& ranges);

        // Fallback chain: when this font lacks a glyph for some codepoint,
        // the renderer walks the chain in order. Each fallback should
        // typically be loaded with the codepoint range it covers (e.g. a
        // primary Latin font + a CJK fallback). The chain is consulted
        // glyph-by-glyph, so a single string can render correctly across
        // multiple scripts.
        void SetFallbackFonts(std::vector<Ref<Font>> fallbacks)
        {
            m_FallbackFonts = std::move(fallbacks);
        }
        [[nodiscard]] const std::vector<Ref<Font>>& GetFallbackFonts() const
        {
            return m_FallbackFonts;
        }

        // Walk this font and its fallback chain for `codepoint`. Returns
        // both the matching font (so the renderer can swap textures) and
        // the glyph data. Returns {nullptr, nullptr} if no font in the
        // chain has the glyph.
        struct GlyphLookup
        {
            const Font* SourceFont = nullptr;
            const SlugGlyphData* Glyph = nullptr;
        };
        [[nodiscard]] GlyphLookup FindGlyphWithFallback(u32 codepoint) const;

        // Measure the local-space width of a single line of text.
        //
        // Iterates UTF-8 codepoints (not bytes), consults the fallback
        // chain for missing glyphs, and honours kerning pairs when both
        // characters resolve against the same source font. Whitespace
        // handling mirrors Renderer2D::DrawString:
        //   '\r' -> skipped
        //   ' '  -> uses space-glyph advance (kerned to next char if any)
        //   '\t' -> 4 × (space advance + kerning)
        //
        // Missing glyphs fall back to '?'; if that's also missing they
        // are skipped (matches Renderer2D::DrawString). `line` is treated
        // as a single line — split on `\n` before calling.
        //
        // `fsScale` is the em-space-to-local-space factor (1 / metricSpan,
        // i.e. 1 / (AscenderY - DescenderY)). `kerning` is added per
        // advance, before scaling. Returns 0.0 if the font is not loaded.
        [[nodiscard]] f32 MeasureLine(std::string_view line, f32 fsScale, f32 kerning) const;

      private:
        // Common initialization path shared by both constructors. Loads
        // glyphs for every codepoint in `ranges` from the on-disk font file.
        void LoadFromFile(const std::filesystem::path& font, const std::vector<FontCodepointRange>& ranges);

        Scope<SlugFontData> m_Data;
        std::string m_Name;
        std::string m_Path;
        bool m_IsLoaded = false;
        std::vector<Ref<Font>> m_FallbackFonts;

      public:
        [[nodiscard]] bool IsLoaded() const
        {
            return m_IsLoaded;
        }
    };
} // namespace OloEngine

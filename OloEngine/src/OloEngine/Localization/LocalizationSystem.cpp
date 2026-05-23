#include "OloEnginePCH.h"
#include "OloEngine/Localization/LocalizationSystem.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Localization/LocalizedTextComponent.h"
#include "OloEngine/Renderer/Font.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine
{
    namespace
    {
        // Resolve the active locale's font configuration into a usable
        // Ref<Font> (with fallback chain attached). Returns nullptr if the
        // active locale declares no font — in that case the system leaves
        // each entity's existing FontAsset alone.
        //
        // Cached at scene-tick scope: the resolution loads font assets
        // from disk, which we don't want to repeat for every entity that
        // shares the active locale. The result lives in static storage
        // and gets refreshed on generation bumps.
        struct CachedLocaleFont
        {
            std::string LocaleCode;
            u64 Generation = 0;
            Ref<Font> Font; // null when locale declares no font
        };

        Ref<Font> ResolveActiveLocaleFont(u64 generation)
        {
            static CachedLocaleFont s_Cache;
            const std::string currentLocale = LocalizationManager::GetCurrentLocale();
            if (s_Cache.LocaleCode == currentLocale && s_Cache.Generation == generation)
                return s_Cache.Font;

            s_Cache.LocaleCode = currentLocale;
            s_Cache.Generation = generation;
            s_Cache.Font.Reset();
            if (currentLocale.empty())
                return s_Cache.Font;

            // Find this locale's LocaleDefinition.
            std::string primaryPath;
            std::vector<std::string> fallbackPaths;
            for (const auto& loc : LocalizationManager::GetAvailableLocales())
            {
                if (loc.Code != currentLocale)
                    continue;
                primaryPath = loc.FontPath;
                fallbackPaths = loc.FontFallbacks;
                break;
            }
            if (primaryPath.empty())
                return s_Cache.Font;

            // Load primary with a sensible default codepoint coverage
            // (Latin-1 + Latin Extended). Callers wanting CJK/Cyrillic
            // coverage typically override via fallback fonts loaded with
            // the wider ranges.
            using namespace FontCodepointRanges;
            const std::vector<FontCodepointRange> primaryRanges = { Latin1, LatinExtA, LatinExtB };
            auto primary = Font::Create(primaryPath, primaryRanges);
            if (!primary || !primary->IsLoaded())
                return s_Cache.Font;

            // Build the fallback chain. Each fallback gets a wider range
            // so a Latin primary + Hiragana/Katakana/CJK fallback
            // produces correct rendering for mixed Japanese-Latin text.
            std::vector<Ref<Font>> fallbacks;
            fallbacks.reserve(fallbackPaths.size());
            for (const auto& fp : fallbackPaths)
            {
                const std::vector<FontCodepointRange> fallbackRanges = {
                    Latin1, LatinExtA, LatinExtB, Cyrillic, Greek,
                    Hebrew, ArabicBasic, Hiragana, Katakana, CJKUnifiedHan
                };
                auto fb = Font::Create(fp, fallbackRanges);
                if (fb && fb->IsLoaded())
                    fallbacks.push_back(std::move(fb));
            }
            primary->SetFallbackFonts(std::move(fallbacks));

            s_Cache.Font = std::move(primary);
            return s_Cache.Font;
        }
    } // namespace

    u32 LocalizationSystem::UpdateLocalizedText(Scene& scene)
    {
        OLO_PROFILE_FUNCTION();

        const u64 current = LocalizationManager::GetGeneration();
        if (current == scene.m_LocalizationGeneration)
            return 0;

        // Resolve the locale's font once per refresh — saves N file loads
        // for N localized text entities sharing the locale.
        auto localeFont = ResolveActiveLocaleFont(current);

        u32 refreshed = 0;
        auto view = scene.GetAllEntitiesWith<LocalizedTextComponent, TextComponent>();
        for (auto e : view)
        {
            const auto& localized = view.template get<LocalizedTextComponent>(e);
            if (localized.LocalizationKey.empty())
                continue;
            auto& text = view.template get<TextComponent>(e);
            text.TextString = LocalizationManager::Get(localized.LocalizationKey);
            if (localeFont)
                text.FontAsset = localeFont;
            ++refreshed;
        }

        scene.m_LocalizationGeneration = current;
        return refreshed;
    }
} // namespace OloEngine

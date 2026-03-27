#include "OloEnginePCH.h"
#include "OloEngine/ImGui/ImGuiFonts.h"

#include "OloEngine/Core/Assert.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine::UI
{
    static std::unordered_map<std::string, ImFont*> s_Fonts;

    void Fonts::Add(const FontConfiguration& config, bool isDefault)
    {
        if (s_Fonts.contains(config.FontName))
        {
            OLO_CORE_WARN_TAG("EditorUI", "Tried to add font with name '{0}' but that name is already taken!", config.FontName);
            return;
        }

        ImFontConfig imguiFontConfig;
        imguiFontConfig.MergeMode = config.MergeWithLast;
        auto& io = ImGui::GetIO();
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            config.FilePath.c_str(),
            config.Size,
            &imguiFontConfig,
            config.GlyphRanges == nullptr ? io.Fonts->GetGlyphRangesDefault() : config.GlyphRanges);
        OLO_CORE_VERIFY(font, "Failed to load font file: {}", config.FilePath);
        s_Fonts[config.FontName] = font;

        if (isDefault)
        {
            io.FontDefault = font;
        }
    }

    ImFont* Fonts::Get(const std::string& fontName)
    {
        auto it = s_Fonts.find(fontName);
        OLO_CORE_VERIFY(it != s_Fonts.end(), "Failed to find font with name '{}'!", fontName);
        return it->second;
    }

    void Fonts::PushFont(const std::string& fontName)
    {
        const auto& io = ImGui::GetIO();
        auto it = s_Fonts.find(fontName);

        if (it == s_Fonts.end())
        {
            ImGui::PushFont(io.FontDefault);
            return;
        }

        ImGui::PushFont(it->second);
    }

    void Fonts::PopFont()
    {
        ImGui::PopFont();
    }

    void Fonts::ClearFonts()
    {
        s_Fonts.clear();
    }

} // namespace OloEngine::UI

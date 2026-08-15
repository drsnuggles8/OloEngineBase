#include "OloEnginePCH.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        // The one live instance. A plain function-local static rather than a
        // file-scope global so construction order is defined for any translation
        // unit that reads it during static init.
        AccessibilitySettings& MutableSettings() noexcept
        {
            static AccessibilitySettings s_Settings;
            return s_Settings;
        }

        // Read a scalar of type T from `node[key]`, leaving `out` untouched when
        // the key is absent or malformed. Absent-key tolerance is what lets a
        // prefs file written by an older build load into a newer one without a
        // version field: YAML is self-describing, so a missing key is a missing
        // key, not a desync (contrast the save-game archive — see
        // docs/agent-rules/binary-format-versioning.md).
        template<typename T>
        void ReadScalar(const YAML::Node& node, const char* key, T& out)
        {
            if (const auto child = node[key]; child && child.IsScalar())
            {
                try
                {
                    out = child.as<T>();
                }
                catch (const YAML::Exception&)
                {
                    OLO_CORE_WARN("Accessibility: ignoring malformed value for '{}'", key);
                }
            }
        }
    } // namespace

    const AccessibilitySettings& Accessibility::Get() noexcept
    {
        return MutableSettings();
    }

    void Accessibility::Set(const AccessibilitySettings& settings) noexcept
    {
        AccessibilitySettings copy = settings;
        SanitizeAccessibilitySettings(copy);
        MutableSettings() = copy;
    }

    void Accessibility::Reset() noexcept
    {
        MutableSettings() = AccessibilitySettings{};
    }

    std::filesystem::path Accessibility::DefaultSettingsPath()
    {
        return std::filesystem::path("accessibility.yaml");
    }

    bool Accessibility::SaveToFile(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        const AccessibilitySettings& s = Get();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Accessibility" << YAML::Value << YAML::BeginMap;

        out << YAML::Key << "SubtitlesEnabled" << YAML::Value << s.SubtitlesEnabled;
        out << YAML::Key << "SubtitleShowSpeaker" << YAML::Value << s.SubtitleShowSpeaker;
        out << YAML::Key << "SubtitleFontSize" << YAML::Value << s.SubtitleFontSize;
        out << YAML::Key << "SubtitleBackgroundOpacity" << YAML::Value << s.SubtitleBackgroundOpacity;

        out << YAML::Key << "UITextScale" << YAML::Value << s.UITextScale;
        out << YAML::Key << "MinimumFontSize" << YAML::Value << s.MinimumFontSize;

        out << YAML::Key << "ColorBlindMode" << YAML::Value << static_cast<i32>(s.ColorBlind);
        out << YAML::Key << "ColorBlindMethod" << YAML::Value << static_cast<i32>(s.ColorBlindMethod);
        out << YAML::Key << "ColorBlindSeverity" << YAML::Value << s.ColorBlindSeverity;

        out << YAML::EndMap; // Accessibility
        out << YAML::EndMap; // root

        std::error_code ec;
        if (const auto parent = path.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                OLO_CORE_ERROR("Accessibility::SaveToFile: cannot create '{}': {}", parent.string(), ec.message());
                return false;
            }
        }

        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
        {
            OLO_CORE_ERROR("Accessibility::SaveToFile: cannot open '{}' for writing", path.string());
            return false;
        }
        file << out.c_str();
        if (!file)
        {
            OLO_CORE_ERROR("Accessibility::SaveToFile: write failed for '{}'", path.string());
            return false;
        }
        return true;
    }

    bool Accessibility::LoadFromFile(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
        {
            // Not an error: a first run simply has no prefs file yet. The caller
            // keeps whatever defaults are already installed.
            return false;
        }

        YAML::Node root;
        try
        {
            root = YAML::LoadFile(path.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("Accessibility::LoadFromFile: parse error in '{}': {}", path.string(), e.what());
            return false;
        }

        const auto node = root["Accessibility"];
        if (!node || !node.IsMap())
        {
            OLO_CORE_WARN("Accessibility::LoadFromFile: '{}' has no Accessibility map", path.string());
            return false;
        }

        // Start from the current settings, not from defaults: a prefs file that
        // omits a key should leave that preference where the user had it.
        AccessibilitySettings s = Get();

        ReadScalar(node, "SubtitlesEnabled", s.SubtitlesEnabled);
        ReadScalar(node, "SubtitleShowSpeaker", s.SubtitleShowSpeaker);
        ReadScalar(node, "SubtitleFontSize", s.SubtitleFontSize);
        ReadScalar(node, "SubtitleBackgroundOpacity", s.SubtitleBackgroundOpacity);

        ReadScalar(node, "UITextScale", s.UITextScale);
        ReadScalar(node, "MinimumFontSize", s.MinimumFontSize);

        i32 mode = static_cast<i32>(s.ColorBlind);
        i32 method = static_cast<i32>(s.ColorBlindMethod);
        ReadScalar(node, "ColorBlindMode", mode);
        ReadScalar(node, "ColorBlindMethod", method);
        s.ColorBlind = static_cast<ColorBlindMode>(mode);
        s.ColorBlindMethod = static_cast<ColorBlindAdaptation>(method);

        ReadScalar(node, "ColorBlindSeverity", s.ColorBlindSeverity);
        // A "ColorBlindGamma" key from an older prefs file is ignored on
        // purpose — the display gamma now comes from PostProcessSettings.

        // Set() sanitizes, so a hand-edited ".nan" or an out-of-range enum
        // lands on the safe default instead of reaching the shader.
        Set(s);
        return true;
    }
} // namespace OloEngine

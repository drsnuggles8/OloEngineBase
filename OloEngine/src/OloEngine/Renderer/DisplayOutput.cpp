#include "OloEnginePCH.h"
#include "OloEngine/Renderer/DisplayOutput.h"

#include "OloEngine/Renderer/BackendSelection.h"

#include <fstream>
#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        // SMPTE ST 2084 constants, exactly as the standard writes them.
        constexpr f32 kPqM1 = 2610.0f / 16384.0f;
        constexpr f32 kPqM2 = 2523.0f / 4096.0f * 128.0f;
        constexpr f32 kPqC1 = 3424.0f / 4096.0f;
        constexpr f32 kPqC2 = 2413.0f / 4096.0f * 32.0f;
        constexpr f32 kPqC3 = 2392.0f / 4096.0f * 32.0f;

        [[nodiscard]] f32 Finite(f32 v, f32 fallback) noexcept
        {
            return std::isfinite(v) ? v : fallback;
        }

        struct State
        {
            DisplayOutputSettings Settings;
            u64 SettingsGeneration = 0;
            DisplayOutputSelection Selection;
            DisplayCapabilities Capabilities;
            f32 CompositionGamma = 2.2f;
        };

        State& MutableState() noexcept
        {
            static State s_State;
            return s_State;
        }

        [[nodiscard]] std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::ranges::transform(out, out.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // A preference by name ("Auto", "SDR", "HDR10", "scRGB"), case-insensitive.
        [[nodiscard]] bool ParsePreference(std::string_view text, DisplayOutputPreference& out)
        {
            const std::string name = Lower(text);
            if (name == "auto")
                out = DisplayOutputPreference::Auto;
            else if (name == "sdr")
                out = DisplayOutputPreference::SDR;
            else if (name == "hdr10")
                out = DisplayOutputPreference::HDR10;
            else if (name == "scrgb")
                out = DisplayOutputPreference::ScRGB;
            else
                return false;
            return true;
        }

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
                    OLO_CORE_WARN("DisplayOutput: ignoring malformed value for '{}'", key);
                }
            }
        }

        // The other HDR route, for a preference whose own route is missing.
        [[nodiscard]] DisplayOutputSelection PickHdrRoute(DisplayOutputMode wanted, const DisplayCapabilities& caps,
                                                          const char* why)
        {
            DisplayOutputSelection selection;
            const bool wantedOffered = wanted == DisplayOutputMode::HDR10 ? caps.OffersHDR10 : caps.OffersScRGB;
            const DisplayOutputMode other = wanted == DisplayOutputMode::HDR10 ? DisplayOutputMode::ScRGB : DisplayOutputMode::HDR10;
            const bool otherOffered = other == DisplayOutputMode::HDR10 ? caps.OffersHDR10 : caps.OffersScRGB;
            if (wantedOffered)
            {
                selection.Mode = wanted;
                selection.Reason = FString(std::string(why) + "; the surface offers " + ToString(wanted));
            }
            else if (otherOffered)
            {
                selection.Mode = other;
                selection.Reason = FString(std::string(why) + "; the surface does not offer " + ToString(wanted) +
                                           ", using " + ToString(other));
            }
            else
            {
                selection.Mode = DisplayOutputMode::SDR;
                selection.Reason = FString(std::string(why) + "; the surface offers no HDR format, using SDR");
            }
            return selection;
        }
    } // namespace

    const char* ToString(const DisplayOutputMode mode) noexcept
    {
        switch (mode)
        {
            case DisplayOutputMode::SDR:
                return "SDR";
            case DisplayOutputMode::HDR10:
                return "HDR10";
            case DisplayOutputMode::ScRGB:
                return "scRGB";
        }
        return "SDR";
    }

    const char* ToString(const DisplayOutputPreference preference) noexcept
    {
        switch (preference)
        {
            case DisplayOutputPreference::Auto:
                return "Auto";
            case DisplayOutputPreference::SDR:
                return "SDR";
            case DisplayOutputPreference::HDR10:
                return "HDR10";
            case DisplayOutputPreference::ScRGB:
                return "scRGB";
        }
        return "Auto";
    }

    const char* ToString(const DisplayHdrState state) noexcept
    {
        switch (state)
        {
            case DisplayHdrState::Unknown:
                return "unknown";
            case DisplayHdrState::Off:
                return "off";
            case DisplayHdrState::On:
                return "on";
        }
        return "unknown";
    }

    void SanitizeDisplayOutputSettings(DisplayOutputSettings& s) noexcept
    {
        if (const i32 preference = static_cast<i32>(s.Preference);
            preference < static_cast<i32>(DisplayOutputPreference::Auto) ||
            preference > static_cast<i32>(DisplayOutputPreference::ScRGB))
        {
            s.Preference = DisplayOutputPreference::Auto;
        }
        s.PaperWhiteNits = std::clamp(Finite(s.PaperWhiteNits, 200.0f), kMinWhiteNits, kMaxWhiteNits);
        s.UIWhiteNits = std::clamp(Finite(s.UIWhiteNits, 200.0f), kMinWhiteNits, kMaxWhiteNits);
        // 0 keeps its meaning ("the display's own"); anything else is a real peak.
        const f32 peak = Finite(s.PeakNitsOverride, 0.0f);
        s.PeakNitsOverride = peak <= 0.0f ? 0.0f : std::clamp(peak, kMinWhiteNits, kMaxPqNits);
    }

    DisplayOutputSelection SelectDisplayOutputMode(const DisplayOutputPreference preference,
                                                   const DisplayCapabilities& caps)
    {
        DisplayOutputSelection selection;
        if (preference == DisplayOutputPreference::SDR)
        {
            selection.Reason = "SDR requested";
            return selection;
        }

        if (!caps.BackendHasHdrRoute)
        {
            selection.PreferenceNotHonoured = preference != DisplayOutputPreference::Auto;
            selection.Reason = FString(std::string("the backend has no HDR presentation route: ") +
                                       (caps.NoRouteReason.IsEmpty() ? std::string("unspecified")
                                                                     : caps.NoRouteReason.ToStdString()));
            return selection;
        }

        if (preference == DisplayOutputPreference::Auto)
        {
            if (caps.OsHdr != DisplayHdrState::On)
            {
                selection.Reason = FString(std::string("Auto: the display's OS HDR mode is ") + ToString(caps.OsHdr) +
                                           ", using SDR");
                return selection;
            }
            // HDR10 first: half scRGB's bandwidth, and the route every HDR
            // display and capture tool understands.
            return PickHdrRoute(DisplayOutputMode::HDR10, caps, "Auto: the display is in OS HDR mode");
        }

        const DisplayOutputMode wanted = preference == DisplayOutputPreference::HDR10 ? DisplayOutputMode::HDR10
                                                                                      : DisplayOutputMode::ScRGB;
        std::string why = std::string(ToString(wanted)) + " requested";
        if (caps.OsHdr == DisplayHdrState::Off)
        {
            // Honoured, but say what the user will see: the compositor converts
            // the HDR swapchain back to SDR for a display not in HDR mode.
            why += " (OS HDR is off on this display: the compositor will tone-map it to SDR)";
        }
        selection = PickHdrRoute(wanted, caps, why.c_str());
        selection.PreferenceNotHonoured = selection.Mode != wanted;
        return selection;
    }

    DisplayMapping ResolveDisplayMapping(const DisplayOutputMode mode, const DisplayOutputSettings& settings,
                                         const DisplayCapabilities& caps) noexcept
    {
        DisplayOutputSettings s = settings;
        SanitizeDisplayOutputSettings(s);

        DisplayMapping mapping;
        mapping.Mode = mode;
        f32 peak = kFallbackPeakNits;
        if (s.PeakNitsOverride > 0.0f)
            peak = s.PeakNitsOverride;
        else if (const f32 reported = Finite(caps.PeakNits, 0.0f); reported > 0.0f)
            peak = reported;
        mapping.PeakNits = std::clamp(peak, kMinWhiteNits, kMaxPqNits);
        mapping.PaperWhiteNits = std::min(s.PaperWhiteNits, mapping.PeakNits);
        mapping.UIWhiteNits = std::min(s.UIWhiteNits, mapping.PeakNits);
        // A display black above a few percent of paper white is a bad report,
        // not a display: it would lift every shadow through the EETF.
        mapping.MinNits = std::clamp(Finite(caps.MinNits, 0.0f), 0.0f, 0.05f * mapping.PaperWhiteNits);
        return mapping;
    }

    f32 PqEncodeNits(const f32 nits) noexcept
    {
        const f32 y = std::clamp(Finite(nits, 0.0f) / kMaxPqNits, 0.0f, 1.0f);
        const f32 ym1 = std::pow(y, kPqM1);
        return std::pow((kPqC1 + kPqC2 * ym1) / (1.0f + kPqC3 * ym1), kPqM2);
    }

    f32 PqDecodeToNits(const f32 signal) noexcept
    {
        const f32 e = std::clamp(Finite(signal, 0.0f), 0.0f, 1.0f);
        const f32 ep = std::pow(e, 1.0f / kPqM2);
        const f32 y = std::pow(std::max(ep - kPqC1, 0.0f) / (kPqC2 - kPqC3 * ep), 1.0f / kPqM1);
        return y * kMaxPqNits;
    }

    glm::vec3 Rec709ToRec2020(const glm::vec3& rgb709) noexcept
    {
        // ITU-R BT.2087-0, equation 4 (rows).
        return {
            0.627403896f * rgb709.r + 0.329283039f * rgb709.g + 0.043313065f * rgb709.b,
            0.069097289f * rgb709.r + 0.919540395f * rgb709.g + 0.011362316f * rgb709.b,
            0.016391439f * rgb709.r + 0.088013308f * rgb709.g + 0.895595253f * rgb709.b,
        };
    }

    f32 Bt2390Eetf(const f32 pqSignal, const f32 minNits, const f32 peakNits) noexcept
    {
        // Source mastering range [0, 10000] nits is the whole PQ range, so the
        // standard's source normalisation is the identity here.
        const f32 e1 = std::clamp(Finite(pqSignal, 0.0f), 0.0f, 1.0f);
        const f32 maxLum = PqEncodeNits(peakNits);
        const f32 minLum = PqEncodeNits(minNits);
        const f32 ks = 1.5f * maxLum - 0.5f;
        f32 e2 = e1;
        if (e1 >= ks && ks < 1.0f)
        {
            const f32 t = (e1 - ks) / (1.0f - ks);
            const f32 t2 = t * t;
            const f32 t3 = t2 * t;
            e2 = (2.0f * t3 - 3.0f * t2 + 1.0f) * ks + (t3 - 2.0f * t2 + t) * (1.0f - ks) + (-2.0f * t3 + 3.0f * t2) * maxLum;
        }
        const f32 oneMinus = 1.0f - e2;
        return e2 + minLum * oneMinus * oneMinus * oneMinus * oneMinus;
    }

    glm::vec3 ToneMapForHdrDisplay(const glm::vec3& sceneLinear, const DisplayMapping& mapping, const bool clipOnly) noexcept
    {
        const glm::vec3 rgb{ std::max(Finite(sceneLinear.r, 0.0f), 0.0f), std::max(Finite(sceneLinear.g, 0.0f), 0.0f),
                             std::max(Finite(sceneLinear.b, 0.0f), 0.0f) };
        const glm::vec3 nits = rgb * mapping.PaperWhiteNits;
        if (clipOnly)
            return glm::min(nits, glm::vec3(mapping.PeakNits));
        const f32 maxNits = std::max(nits.r, std::max(nits.g, nits.b));
        if (maxNits <= 0.0f)
        {
            // Pure black still takes the black lift, like any other pixel.
            return glm::vec3(PqDecodeToNits(Bt2390Eetf(0.0f, mapping.MinNits, mapping.PeakNits)));
        }
        const f32 mapped = PqDecodeToNits(Bt2390Eetf(PqEncodeNits(std::min(maxNits, kMaxPqNits)), mapping.MinNits, mapping.PeakNits));
        return nits * (mapped / maxNits);
    }

    glm::vec3 NitsToComposition(const glm::vec3& nits, const DisplayMapping& mapping, const f32 gamma) noexcept
    {
        const f32 g = std::max(Finite(gamma, 2.2f), 0.1f);
        const glm::vec3 relative = glm::max(nits / mapping.UIWhiteNits, glm::vec3(0.0f));
        return glm::pow(relative, glm::vec3(1.0f / g));
    }

    glm::vec3 EncodeCompositionForDisplay(const glm::vec3& composition, const DisplayMapping& mapping, const f32 gamma) noexcept
    {
        if (!mapping.IsHdr())
            return composition;
        const f32 g = std::max(Finite(gamma, 2.2f), 0.1f);
        const glm::vec3 nits709 = glm::pow(glm::max(composition, glm::vec3(0.0f)), glm::vec3(g)) * mapping.UIWhiteNits;
        if (mapping.Mode == DisplayOutputMode::HDR10)
        {
            const glm::vec3 nits2020 = glm::clamp(Rec709ToRec2020(nits709), glm::vec3(0.0f), glm::vec3(mapping.PeakNits));
            return { PqEncodeNits(nits2020.r), PqEncodeNits(nits2020.g), PqEncodeNits(nits2020.b) };
        }
        return glm::min(nits709, glm::vec3(mapping.PeakNits)) / kScRgbReferenceNits;
    }

    const DisplayOutputSettings& DisplayOutput::GetSettings() noexcept
    {
        return MutableState().Settings;
    }

    void DisplayOutput::SetSettings(const DisplayOutputSettings& settings) noexcept
    {
        DisplayOutputSettings copy = settings;
        SanitizeDisplayOutputSettings(copy);
        State& state = MutableState();
        if (copy == state.Settings)
            return;
        state.Settings = copy;
        ++state.SettingsGeneration;
    }

    u64 DisplayOutput::GetSettingsGeneration() noexcept
    {
        return MutableState().SettingsGeneration;
    }

    void DisplayOutput::PublishActive(const DisplayOutputSelection& selection, const DisplayCapabilities& caps)
    {
        State& state = MutableState();
        state.Selection = selection;
        state.Capabilities = caps;
    }

    const DisplayOutputSelection& DisplayOutput::GetActiveSelection() noexcept
    {
        return MutableState().Selection;
    }

    const DisplayCapabilities& DisplayOutput::GetActiveCapabilities() noexcept
    {
        return MutableState().Capabilities;
    }

    DisplayMapping DisplayOutput::GetActiveMapping() noexcept
    {
        const State& state = MutableState();
        return ResolveDisplayMapping(state.Selection.Mode, state.Settings, state.Capabilities);
    }

    void DisplayOutput::SetCompositionGamma(const f32 gamma) noexcept
    {
        MutableState().CompositionGamma = std::clamp(Finite(gamma, 2.2f), 0.1f, 5.0f);
    }

    f32 DisplayOutput::GetCompositionGamma() noexcept
    {
        return MutableState().CompositionGamma;
    }

    void DisplayOutput::Reset() noexcept
    {
        State& state = MutableState();
        const u64 generation = state.SettingsGeneration + 1u;
        state = State{};
        state.SettingsGeneration = generation;
    }

    std::filesystem::path DisplayOutput::DefaultSettingsPath()
    {
        return DefaultRendererConfigPath().parent_path() / "display.yaml";
    }

    bool DisplayOutput::SaveToFile(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        const DisplayOutputSettings& s = GetSettings();
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "DisplayOutput" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Preference" << YAML::Value << ToString(s.Preference);
        out << YAML::Key << "PaperWhiteNits" << YAML::Value << s.PaperWhiteNits;
        out << YAML::Key << "UIWhiteNits" << YAML::Value << s.UIWhiteNits;
        out << YAML::Key << "PeakNitsOverride" << YAML::Value << s.PeakNitsOverride;
        out << YAML::EndMap;
        out << YAML::EndMap;

        std::error_code ec;
        if (const auto parent = path.parent_path(); !parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                OLO_CORE_ERROR("DisplayOutput::SaveToFile: cannot create '{}': {}", parent.string(), ec.message());
                return false;
            }
        }
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
        {
            OLO_CORE_ERROR("DisplayOutput::SaveToFile: cannot open '{}' for writing", path.string());
            return false;
        }
        file << out.c_str();
        if (!file)
        {
            OLO_CORE_ERROR("DisplayOutput::SaveToFile: write failed for '{}'", path.string());
            return false;
        }
        return true;
    }

    bool DisplayOutput::LoadFromFile(const std::filesystem::path& path)
    {
        OLO_PROFILE_FUNCTION();

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return false; // first run: no prefs yet, defaults stay

        YAML::Node root;
        try
        {
            root = YAML::LoadFile(path.string());
        }
        catch (const YAML::Exception& e)
        {
            OLO_CORE_ERROR("DisplayOutput::LoadFromFile: parse error in '{}': {}", path.string(), e.what());
            return false;
        }
        const auto node = root["DisplayOutput"];
        if (!node || !node.IsMap())
        {
            OLO_CORE_WARN("DisplayOutput::LoadFromFile: '{}' has no DisplayOutput map", path.string());
            return false;
        }

        // From the current settings, so an omitted key keeps the user's value.
        DisplayOutputSettings s = GetSettings();
        if (const auto preference = node["Preference"]; preference && preference.IsScalar())
        {
            if (!ParsePreference(preference.Scalar(), s.Preference))
                OLO_CORE_WARN("DisplayOutput: unknown Preference '{}', keeping {}", preference.Scalar(), ToString(s.Preference));
        }
        ReadScalar(node, "PaperWhiteNits", s.PaperWhiteNits);
        ReadScalar(node, "UIWhiteNits", s.UIWhiteNits);
        ReadScalar(node, "PeakNitsOverride", s.PeakNitsOverride);
        SetSettings(s); // sanitizes
        return true;
    }
} // namespace OloEngine

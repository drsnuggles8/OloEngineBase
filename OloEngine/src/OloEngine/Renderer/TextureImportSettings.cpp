#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureImportSettings.h"

#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kSidecarVersion = 1;

        // Spelling <-> enum tables. Kept as one table per enum so the parser and the
        // emitter cannot drift apart, and so an unknown spelling is rejected rather than
        // silently mapped to Auto.
        constexpr std::array<std::pair<std::string_view, TextureImportSettings::FormatChoice>, 5> kFormatNames = { {
            { "Auto", TextureImportSettings::FormatChoice::Auto },
            { "BC7", TextureImportSettings::FormatChoice::BC7 },
            { "BC5", TextureImportSettings::FormatChoice::BC5 },
            { "BC6H", TextureImportSettings::FormatChoice::BC6H },
            { "BC6HSigned", TextureImportSettings::FormatChoice::BC6HSigned },
        } };

        constexpr std::array<std::pair<std::string_view, TextureImportSettings::ColorSpaceChoice>, 3> kColorSpaceNames = { {
            { "Auto", TextureImportSettings::ColorSpaceChoice::Auto },
            { "Linear", TextureImportSettings::ColorSpaceChoice::Linear },
            { "sRGB", TextureImportSettings::ColorSpaceChoice::SRGB },
        } };

        template<typename Table, typename Value>
        bool LookupName(const Table& table, std::string_view name, Value& out)
        {
            for (const auto& [text, value] : table)
            {
                if (text == name)
                {
                    out = value;
                    return true;
                }
            }
            return false;
        }

        template<typename Table, typename Value>
        std::string_view NameOf(const Table& table, Value value)
        {
            for (const auto& [text, candidate] : table)
            {
                if (candidate == value)
                    return text;
            }
            return "Auto";
        }
    } // namespace

    namespace TextureImport
    {
        std::string SidecarPathFor(std::string_view sourceImagePath)
        {
            std::string path(sourceImagePath);
            path.append(kSidecarExtension);
            return path;
        }

        bool Parse(std::string_view yaml, TextureImportSettings& out)
        {
            out = TextureImportSettings{};
            try
            {
                const YAML::Node root = YAML::Load(std::string(yaml));
                const YAML::Node node = root["TextureImportSettings"];
                if (!node || !node.IsMap())
                {
                    OLO_CORE_ERROR("TextureImport::Parse - missing 'TextureImportSettings' map");
                    return false;
                }

                // An unknown version is a hard error rather than a best-effort read: a
                // future field the cook silently ignores is exactly how a texture ships
                // in the wrong format without anyone noticing.
                if (const YAML::Node version = node["Version"]; version && version.as<u32>(0u) != kSidecarVersion)
                {
                    OLO_CORE_ERROR("TextureImport::Parse - unsupported sidecar version {} (expected {})",
                                   version.as<u32>(0u), kSidecarVersion);
                    return false;
                }

                if (const YAML::Node format = node["Format"]; format)
                {
                    if (!LookupName(kFormatNames, format.as<std::string>(std::string{}), out.Format))
                    {
                        OLO_CORE_ERROR("TextureImport::Parse - unknown Format '{}'", format.as<std::string>(std::string{}));
                        return false;
                    }
                }
                if (const YAML::Node colorSpace = node["ColorSpace"]; colorSpace)
                {
                    if (!LookupName(kColorSpaceNames, colorSpace.as<std::string>(std::string{}), out.ColorSpace))
                    {
                        OLO_CORE_ERROR("TextureImport::Parse - unknown ColorSpace '{}'", colorSpace.as<std::string>(std::string{}));
                        return false;
                    }
                }
                if (const YAML::Node generateMips = node["GenerateMips"]; generateMips)
                    out.GenerateMips = generateMips.as<bool>(true);

                return true;
            }
            catch (const YAML::Exception& e)
            {
                OLO_CORE_ERROR("TextureImport::Parse - YAML error: {}", e.what());
                out = TextureImportSettings{};
                return false;
            }
        }

        std::string Emit(const TextureImportSettings& settings)
        {
            std::ostringstream stream;
            stream << "TextureImportSettings:\n";
            stream << "  Version: " << kSidecarVersion << "\n";
            stream << "  Format: " << NameOf(kFormatNames, settings.Format) << "\n";
            stream << "  ColorSpace: " << NameOf(kColorSpaceNames, settings.ColorSpace) << "\n";
            if (settings.GenerateMips.has_value())
                stream << "  GenerateMips: " << (*settings.GenerateMips ? "true" : "false") << "\n";
            return stream.str();
        }

        bool LoadForImage(std::string_view sourceImagePath, TextureImportSettings& out)
        {
            out = TextureImportSettings{};
            const std::string sidecar = SidecarPathFor(sourceImagePath);

            // A missing sidecar is the normal case, not a failure: report it by the
            // return value and stay silent.
            std::error_code ec;
            if (!std::filesystem::exists(sidecar, ec) || ec)
                return false;

            std::ifstream file(sidecar, std::ios::binary);
            if (!file)
            {
                OLO_CORE_ERROR("TextureImport::LoadForImage - cannot open '{}'", sidecar);
                return false;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            if (!Parse(buffer.str(), out))
            {
                OLO_CORE_ERROR("TextureImport::LoadForImage - '{}' is malformed; cooking '{}' with automatic settings",
                               sidecar, std::string(sourceImagePath));
                return false;
            }
            return true;
        }

        bool SaveForImage(std::string_view sourceImagePath, const TextureImportSettings& settings)
        {
            const std::string sidecar = SidecarPathFor(sourceImagePath);
            std::ofstream file(sidecar, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                OLO_CORE_ERROR("TextureImport::SaveForImage - cannot open '{}' for writing", sidecar);
                return false;
            }
            const std::string text = Emit(settings);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
            {
                OLO_CORE_ERROR("TextureImport::SaveForImage - short write to '{}'", sidecar);
                return false;
            }
            return true;
        }
    } // namespace TextureImport
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Build/LinuxDesktopLauncher.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

namespace OloEngine
{
    namespace
    {
        constexpr std::string_view LauncherScriptName = "OloGameLauncher.sh";
        constexpr std::string_view IconDirectoryName = "icons";
        constexpr std::string_view IconFileStem = "game-icon";

        std::string EscapeDesktopString(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (const char character : value)
            {
                switch (character)
                {
                    case '\\':
                        escaped += "\\\\";
                        break;
                    case '\n':
                        escaped += "\\n";
                        break;
                    case '\r':
                        escaped += "\\r";
                        break;
                    case '\t':
                        escaped += "\\t";
                        break;
                    default:
                        escaped += character;
                        break;
                }
            }
            return escaped;
        }

        std::string EscapeForQuotedExec(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size() * 2);
            for (const char character : value)
            {
                if (character == '\\' || character == '"' || character == '`' || character == '$')
                {
                    escaped += '\\';
                }
                escaped += character;
            }
            return escaped;
        }

        std::string ToLowerExtension(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char character)
                                   { return static_cast<char>(std::tolower(character)); });
            return extension;
        }

        bool IsSupportedLinuxIcon(const std::filesystem::path& iconPath)
        {
            const auto extension = ToLowerExtension(iconPath);
            return extension == ".png" || extension == ".svg" || extension == ".xpm";
        }

        bool HasRecognizableLinuxIconContent(const std::filesystem::path& iconPath)
        {
            std::ifstream input(iconPath, std::ios::binary);
            if (!input.is_open())
            {
                return false;
            }

            const auto extension = ToLowerExtension(iconPath);
            if (extension == ".png")
            {
                constexpr std::array<char, 8> signature{ '\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n' };
                std::array<char, signature.size()> bytes{};
                input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                return input.gcount() == static_cast<std::streamsize>(signature.size()) && bytes == signature;
            }

            std::string prefix(4096, '\0');
            input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            prefix.resize(static_cast<sizet>(input.gcount()));
            std::ranges::transform(prefix, prefix.begin(), [](const unsigned char character)
                                   { return static_cast<char>(std::tolower(character)); });
            return extension == ".svg" ? prefix.find("<svg") != std::string::npos
                                       : prefix.find("xpm") != std::string::npos;
        }

        bool WriteTextFile(const std::filesystem::path& path, const std::string& contents, std::string& errorMessage)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                errorMessage = "Failed to write launcher file: " + path.string();
                return false;
            }
            output << contents;
            if (!output.good())
            {
                errorMessage = "Failed while writing launcher file: " + path.string();
                return false;
            }
            return true;
        }
    } // namespace

    bool StageLinuxDesktopLauncher(const std::filesystem::path& executablePath,
                                   const std::filesystem::path& iconPath,
                                   const std::string& gameName,
                                   LinuxDesktopLauncherArtifacts& artifacts,
                                   std::string& errorMessage)
    {
        artifacts = {};
        if (!std::filesystem::is_regular_file(executablePath))
        {
            errorMessage = "Executable not found: " + executablePath.string();
            return false;
        }
        const std::filesystem::path nameAsPath{ gameName };
        if (gameName.empty() || nameAsPath.is_absolute() || nameAsPath.filename().string() != gameName ||
            gameName.find("..") != std::string::npos)
        {
            errorMessage = "Game name is not a safe launcher filename: " + gameName;
            return false;
        }

        const auto packageDirectory = executablePath.parent_path();
        const auto desktopEntryPath = packageDirectory / (gameName + ".desktop");
        const auto wrapperScriptPath = packageDirectory / LauncherScriptName;

        std::filesystem::path packagedIconPath;
        if (!iconPath.empty())
        {
            if (!std::filesystem::is_regular_file(iconPath))
            {
                errorMessage = "Linux icon file not found: " + iconPath.string();
                return false;
            }
            if (!IsSupportedLinuxIcon(iconPath))
            {
                errorMessage = "Linux icons must use PNG, SVG, or XPM: " + iconPath.string();
                return false;
            }
            if (!HasRecognizableLinuxIconContent(iconPath))
            {
                errorMessage = "Linux icon content does not match its extension: " + iconPath.string();
                return false;
            }

            packagedIconPath = packageDirectory / IconDirectoryName / (std::string(IconFileStem) + ToLowerExtension(iconPath));
            std::error_code copyError;
            std::filesystem::create_directories(packagedIconPath.parent_path(), copyError);
            if (!copyError)
            {
                std::filesystem::copy_file(iconPath, packagedIconPath, std::filesystem::copy_options::overwrite_existing, copyError);
            }
            if (copyError)
            {
                errorMessage = "Failed to package Linux icon: " + copyError.message();
                return false;
            }
        }

        // Desktop Entry Specification §7 does not allow a relative executable.
        // `%k` is the launcher's own location, so pass it as its required standalone
        // argument to a fixed system shell; the wrapper then derives its directory.
        const std::string bootstrapCommand = "exec \"$(dirname -- \"$1\")/" + std::string(LauncherScriptName) + "\"";
        const std::string desktopEntry = "[Desktop Entry]\n"
                                         "Type=Application\n"
                                         "Name=" +
                                         EscapeDesktopString(gameName) + "\n"
                                                                         "Exec=/bin/sh -c \"" +
                                         EscapeForQuotedExec(bootstrapCommand) + "\" olo-launcher %k\n"
                                                                                 "Categories=Game;\n"
                                                                                 "Terminal=false\n";
        if (!WriteTextFile(desktopEntryPath, desktopEntry, errorMessage))
        {
            return false;
        }

        // Icon= cannot resolve a relative path. The raw desktop entry remains
        // launchable after a move; each wrapper launch refreshes its Icon= with
        // the packaged icon's newly derived absolute path.
        const std::string wrapperScript = "#!/bin/sh\n"
                                          "set -eu\n"
                                          "launcher_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
                                          "desktop_entry=\"$launcher_dir/" +
                                          EscapeForQuotedExec(desktopEntryPath.filename().string()) + "\"\n"
                                                                                                      "temporary_entry=\"$desktop_entry.tmp\"\n"
                                                                                                      "if (desktop_mode=$(stat -c '%a' -- \"$desktop_entry\") && cat > \"$temporary_entry\" <<'OLO_DESKTOP_ENTRY'\n" +
                                          desktopEntry +
                                          "OLO_DESKTOP_ENTRY\n" +
                                          (packagedIconPath.empty() ? std::string{} : "printf 'Icon=%s\\n' \"$launcher_dir/" + EscapeForQuotedExec(std::string(IconDirectoryName) + "/" + packagedIconPath.filename().string()) + "\" >> \"$temporary_entry\"\n") +
                                          "chmod \"$desktop_mode\" -- \"$temporary_entry\" && mv -f -- \"$temporary_entry\" \"$desktop_entry\") 2>/dev/null; then :; fi\n"
                                          "exec \"$launcher_dir/" +
                                          EscapeForQuotedExec(executablePath.filename().string()) + "\" \"$@\"\n";
        if (!WriteTextFile(wrapperScriptPath, wrapperScript, errorMessage))
        {
            return false;
        }

        artifacts.DesktopEntryPath = desktopEntryPath;
        artifacts.WrapperScriptPath = wrapperScriptPath;
        artifacts.PackagedIconPath = packagedIconPath;
        return true;
    }
} // namespace OloEngine

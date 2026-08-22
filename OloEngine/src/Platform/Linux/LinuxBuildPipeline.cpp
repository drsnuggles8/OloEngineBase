// Linux implementation of BuildPipelinePlatform.
// Icon embedding in ELF binaries is not currently supported; Linux desktop
// environments pick up an application's icon and launch command from a
// freedesktop.org .desktop entry instead (#891), which this file writes.

#include "OloEnginePCH.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include "OloEngine/Core/Log.h"

#include <fstream>

namespace OloEngine::BuildPipelinePlatform
{
    bool EmbedCustomIcon(const std::filesystem::path& /*exePath*/,
                         const std::filesystem::path& /*iconPath*/,
                         std::string& outError)
    {
        outError = "Icon embedding is not supported on Linux";
        return false;
    }

    namespace
    {
        // Desktop Entry Specification quoting for a value inside a quoted Exec
        // field: a backslash, double quote, backtick or dollar sign must be
        // backslash-escaped, or the entry is malformed and desktop environments
        // may refuse to parse it.
        // https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
        std::string EscapeForQuotedExecValue(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (char c : value)
            {
                if (c == '\\' || c == '"' || c == '`' || c == '$')
                {
                    escaped += '\\';
                }
                escaped += c;
            }
            return escaped;
        }

        // The Exec key also reserves `%` for field codes (%f, %u, ...) — a
        // literal percent must be doubled to `%%` or a GameName containing one
        // (e.g. "Game%Game", which passes GameBuildPipeline's name validation)
        // would be misinterpreted as an undefined field code. This is a
        // separate, later pass: general escaping happens first, then this.
        std::string EscapePercentForExecField(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (char c : value)
            {
                if (c == '%')
                {
                    escaped += '%';
                }
                escaped += c;
            }
            return escaped;
        }

        // Name is a plain localestring — no `%`/backtick/`$` field-code or
        // shell-quoting concerns, just the general backslash/quote escaping.
        std::string EscapeForDesktopName(const std::string& value)
        {
            std::string escaped;
            escaped.reserve(value.size());
            for (char c : value)
            {
                if (c == '\\')
                {
                    escaped += '\\';
                }
                escaped += c;
            }
            return escaped;
        }
    } // namespace

    bool WriteDesktopEntry(const std::filesystem::path& exePath,
                           const std::filesystem::path& iconPath,
                           const std::string& gameName,
                           std::string& outError)
    {
        // Known limitation: Exec= embeds exePath's build-time absolute path,
        // so relocating the finished game folder to a different path (or a
        // different machine) breaks the launcher until it's regenerated —
        // a proper fix is an install-time step, out of scope for #891, whose
        // ask is a working .desktop entry for a build run in place.
        if (!std::filesystem::exists(exePath))
        {
            outError = "Executable not found: " + exePath.string();
            return false;
        }

        // The executable bit is expected to already be set here —
        // GameBuildPipeline::CopyRuntimeExecutable marks the copied runtime
        // executable runnable before this is called, since copy_file does not
        // reliably preserve the executable bit across filesystems.

        const auto desktopPath = exePath.parent_path() / (gameName + ".desktop");
        std::ofstream out(desktopPath, std::ios::trunc);
        if (!out.is_open())
        {
            outError = "Failed to create .desktop file: " + desktopPath.string();
            return false;
        }

        out << "[Desktop Entry]\n";
        out << "Type=Application\n";
        out << "Name=" << EscapeForDesktopName(gameName) << "\n";
        out << "Exec=\"" << EscapePercentForExecField(EscapeForQuotedExecValue(exePath.string())) << "\"\n";
        if (!iconPath.empty())
        {
            // freedesktop Icon= expects a PNG/SVG/XPM file or icon-theme name,
            // not a Windows .ico — GameBuildSettings::IconPath is shared across
            // both targets, so a Windows-authored icon typically won't render
            // here. Non-fatal: desktop environments fall back to a generic icon.
            out << "Icon=" << iconPath.string() << "\n";
        }
        out << "Categories=Game;\n";
        out << "Terminal=false\n";
        out.close();

        // Many desktop environments only treat a .desktop file as "trusted"
        // (rather than prompting "Untrusted application launcher") once it is
        // itself marked executable.
        std::error_code desktopPermEc;
        std::filesystem::permissions(desktopPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, desktopPermEc);
        if (desktopPermEc)
        {
            OLO_CORE_WARN("[GameBuild] Failed to mark {} executable: {}", desktopPath.string(), desktopPermEc.message());
        }

        OLO_CORE_INFO("[GameBuild] Linux desktop entry written: {}", desktopPath.string());
        return true;
    }

} // namespace OloEngine::BuildPipelinePlatform

#endif // OLO_PLATFORM_LINUX

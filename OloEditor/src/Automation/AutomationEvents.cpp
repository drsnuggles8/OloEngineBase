#include "OloEnginePCH.h"
#include "Automation/AutomationEvents.h"

#include <string>
#include <system_error>

namespace OloEngine::Automation::Events
{
    std::string ProjectRelativePath(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        if (path.empty())
            return {};
        if (!root.empty())
        {
            // Lexical, not std::filesystem::relative: that one canonicalizes both
            // sides with filesystem calls on every event, and both callers already
            // hand in absolute paths. lexically_relative answers ".." for a path
            // outside the root and "" for one on another drive; both mean "not
            // inside", and neither may be recorded as if it were a project path.
            if (const std::filesystem::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
                !relative.empty() && relative.native().rfind(std::filesystem::path("..").native(), 0) != 0)
            {
                return relative.generic_string();
            }
        }
        return path.filename().generic_string();
    }

    u64 PublishSceneSaved(std::string_view sceneName, const std::filesystem::path& path,
                          const std::filesystem::path& projectRoot, bool changed)
    {
        const std::string relative = ProjectRelativePath(path, projectRoot);
        DiagnosticEventData data;
        data.Set("scene", sceneName).Set("path", relative).Set("changed", changed);
        return DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::SceneSave,
            std::string(changed ? "Saved scene '" : "Scene already saved '") + std::string(sceneName) + "'", 0,
            path.filename().string(), data);
    }

    u64 PublishSceneDirty(std::string_view sceneName, bool dirty)
    {
        DiagnosticEventData data;
        data.Set("scene", sceneName).Set("dirty", dirty);
        return DiagnosticsEventLog::Get().Record(
            DiagnosticEventCategory::SceneDirty,
            "Scene '" + std::string(sceneName) + (dirty ? "' has unsaved changes" : "' is clean again"), 0,
            std::string(sceneName), data);
    }

    u64 PublishAssetImported(u64 handle, std::string_view assetType, const std::filesystem::path& path,
                             const std::filesystem::path& projectRoot, std::string_view source)
    {
        const std::string relative = ProjectRelativePath(path, projectRoot);
        DiagnosticEventData data;
        data.Set("asset", relative).Set("type", assetType).Set("handle", handle).Set("source", source);
        return DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::AssetImport,
                                                 "Imported " + std::string(assetType) + " '" +
                                                     path.filename().string() + "'",
                                                 handle, relative, data);
    }

    u64 PublishCompileFinished(std::string_view kind, std::string_view target, bool ok, u32 errors, u32 warnings,
                               f64 seconds)
    {
        // The record itself lives in the engine header, because two of the three
        // compile paths (the script assembly, the shader libraries) are engine
        // code; forwarding keeps one shape for all three.
        return DiagnosticsEventLog::Get().RecordCompileFinished(kind, target, ok, errors, warnings, seconds);
    }

    bool EmitsCompletionEvent(const AutomationCommand& command)
    {
        if (!command.Annotations.is_object())
            return true;
        const auto hint = command.Annotations.find("readOnlyHint");
        if (hint == command.Annotations.end() || !hint->is_boolean())
            return true;
        return !hint->get<bool>();
    }

    u64 PublishCommandCompleted(const AutomationCommand& command, const AutomationResult& result, f64 durationMs)
    {
        DiagnosticEventData data;
        data.Set("command", command.Name)
            .Set("ok", !result.IsError)
            .Set("durationMs", durationMs)
            .Set("projectWrite", command.ProjectWrite)
            .Set("toolset", command.Toolset);
        return DiagnosticsEventLog::Get().Record(DiagnosticEventCategory::CommandCompleted,
                                                 "Command " + command.Name + (result.IsError ? " failed" : " completed"),
                                                 0, command.Name, data);
    }
} // namespace OloEngine::Automation::Events

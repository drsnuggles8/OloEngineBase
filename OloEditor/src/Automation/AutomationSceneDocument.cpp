#include "OloEnginePCH.h"
#include "Automation/AutomationSceneDocument.h"

#include "Automation/AutomationFileWrite.h"

#include "OloEngine/Scene/SceneSerializer.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace OloEngine::Automation
{
    SceneDocumentSettings SceneDocumentSettings::Capture(const Scene& scene)
    {
        return { scene.GetPostProcessSettings(), scene.GetSnowSettings(), scene.GetWindSettings(),
                 scene.GetSnowAccumulationSettings(), scene.GetSnowEjectaSettings(),
                 scene.GetPrecipitationSettings(), scene.GetFogSettings() };
    }

    void SceneDocumentSettings::Apply(Scene& scene) const
    {
        scene.SetPostProcessSettings(PostProcess);
        scene.SetSnowSettings(Snow);
        scene.SetWindSettings(Wind);
        scene.SetSnowAccumulationSettings(SnowAccumulation);
        scene.SetSnowEjectaSettings(SnowEjecta);
        scene.SetPrecipitationSettings(Precipitation);
        scene.SetFogSettings(Fog);
    }

    SceneDocumentSnapshot CaptureSceneDocument(const Ref<Scene>& scene, const std::filesystem::path& path)
    {
        if (!scene)
            throw std::runtime_error("No editor scene is available.");
        SceneDocumentSnapshot result;
        result.SceneRef = scene;
        result.Path = path;
        result.Name = scene->GetName();
        result.AuthoredSettings = SceneDocumentSettings::Capture(*scene);
        result.RenderedSettings = result.AuthoredSettings;
        return result;
    }

    void ApplySceneDocument(const SceneDocumentSnapshot& document)
    {
        if (!document.SceneRef)
            throw std::runtime_error("Cannot install an empty scene document.");
        auto scene = document.SceneRef;
        scene->SetName(document.Name);
        document.AuthoredSettings.Apply(*scene);
    }

    namespace
    {
        // Atomic, guarded file replacement lives in AutomationFileWrite.h -- the
        // asset commands (#1128) need byte-identical behaviour, and two copies of
        // this guard would be two chances to get it subtly different.
        std::filesystem::path ResolveDestination(const SceneDocumentAccess& access,
                                                 const std::filesystem::path& requested)
        {
            if (requested.empty())
                throw std::runtime_error("This scene has no path. Use olo_scene_save_as with an explicit .olo path.");
            if (requested.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
                throw std::runtime_error("Scene path contains a null character.");
            auto path = requested;
            if (path.is_relative())
            {
                const auto root = access.AssetDirectory ? access.AssetDirectory() : std::filesystem::path{};
                if (root.empty())
                    throw std::runtime_error("Relative scene paths require an active project asset directory.");
                path = root / path;
            }
            path = std::filesystem::absolute(path).lexically_normal();
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char ch)
                                   { return static_cast<char>(std::tolower(ch)); });
            if (extension != ".olo" && extension != ".scene")
                throw std::runtime_error("Scene destination must end in .olo or .scene.");
            if (!std::filesystem::is_directory(path.parent_path()))
                throw std::runtime_error("Scene destination directory does not exist: " + path.parent_path().string());
            return path;
        }

        std::string SerializeDocument(const SceneDocumentSnapshot& document)
        {
            const auto original = CaptureSceneDocument(document.SceneRef);
            try
            {
                ApplySceneDocument(document);
                std::string bytes = SceneSerializer(document.SceneRef).SerializeToYAML();
                ApplySceneDocument(original);
                return bytes;
            }
            catch (...)
            {
                ApplySceneDocument(original);
                throw;
            }
        }

        class SceneDocumentCommand final : public EditorCommand
        {
          public:
            SceneDocumentCommand(SceneDocumentAccess access, SceneDocumentSnapshot before,
                                 SceneDocumentSnapshot after)
                : m_Access(std::move(access)), m_Before(std::move(before)), m_After(std::move(after))
            {
            }

            void Execute() override
            {
                m_Access.Install(m_After);
            }
            void Undo() override
            {
                m_Access.Install(m_Before);
            }
            [[nodiscard]] std::string GetDescription() const override
            {
                return "New Scene";
            }

          private:
            SceneDocumentAccess m_Access;
            SceneDocumentSnapshot m_Before;
            SceneDocumentSnapshot m_After;
        };

        class SaveSceneDocumentCommand final : public EditorCommand
        {
          public:
            SaveSceneDocumentCommand(SceneDocumentAccess access, SceneDocumentSnapshot before,
                                     SceneDocumentSnapshot after, FileContents oldBytes, std::string newBytes)
                : m_Access(std::move(access)), m_Before(std::move(before)), m_After(std::move(after)),
                  m_OldBytes(std::move(oldBytes)), m_NewBytes(std::move(newBytes))
            {
            }

            void Execute() override
            {
                ReplaceFileContents(m_After.Path, m_OldBytes, m_NewBytes);
                try
                {
                    m_Access.Install(m_After);
                }
                catch (...)
                {
                    ReplaceFileContents(m_After.Path, m_NewBytes, m_OldBytes);
                    throw;
                }
            }

            void Undo() override
            {
                ReplaceFileContents(m_After.Path, m_NewBytes, m_OldBytes);
                try
                {
                    m_Access.Install(m_Before);
                }
                catch (...)
                {
                    ReplaceFileContents(m_After.Path, m_OldBytes, m_NewBytes);
                    throw;
                }
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return "Save Scene";
            }

          private:
            SceneDocumentAccess m_Access;
            SceneDocumentSnapshot m_Before;
            SceneDocumentSnapshot m_After;
            FileContents m_OldBytes;
            FileContents m_NewBytes;
        };

        void RequireAccess(const SceneDocumentAccess& access)
        {
            if (!access.Capture || !access.Install)
                throw std::runtime_error("Scene document lifecycle is unavailable on this host.");
        }
    } // namespace

    void NewSceneDocument(const SceneDocumentAccess& access, CommandHistory& history, const std::string& name)
    {
        RequireAccess(access);
        auto before = access.Capture();
        auto scene = Ref<Scene>::Create();
        if (!name.empty())
            scene->SetName(name);
        auto after = CaptureSceneDocument(scene);
        history.Execute(std::make_unique<SceneDocumentCommand>(access, std::move(before), std::move(after)), true);
    }

    bool SaveSceneDocument(const SceneDocumentAccess& access, CommandHistory& history, const std::filesystem::path& path)
    {
        RequireAccess(access);
        auto before = access.Capture();
        auto after = access.PrepareSave ? access.PrepareSave(before) : before;
        after.Path = ResolveDestination(access, path.empty() ? before.Path : path);
        if (!path.empty())
            after.Name = after.Path.stem().string();
        auto oldBytes = ReadFileContents(after.Path);
        auto newBytes = SerializeDocument(after);
        if (oldBytes && *oldBytes == newBytes && before.Path == after.Path && before.Name == after.Name &&
            SerializeDocument(before) == newBytes)
        {
            RequireFileContents(after.Path, oldBytes);
            history.MarkSaved();
            return false;
        }
        history.Execute(std::make_unique<SaveSceneDocumentCommand>(access, std::move(before), std::move(after),
                                                                   std::move(oldBytes), std::move(newBytes)),
                        true);
        return true;
    }
} // namespace OloEngine::Automation

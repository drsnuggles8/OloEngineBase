#pragma once

#include <filesystem>
#include <string>

namespace OloEngine
{
    class ShaderEditorPanel
    {
      public:
        ShaderEditorPanel() = default;

        void OnImGuiRender(bool* p_open = nullptr);

        // Open a .glsl file for editing
        void OpenFile(const std::filesystem::path& filepath);

        // The file the panel has actually loaded (empty = none). olo_asset_open (issue #607)
        // reads this back instead of trusting that an Open* call succeeded.
        [[nodiscard]] const std::filesystem::path& GetLoadedFilePath() const
        {
            return m_CurrentFilePath;
        }
        [[nodiscard]] bool HasUnsavedChanges() const
        {
            return m_Dirty;
        }

        bool Save();

      private:
        void LoadFileContents(const std::filesystem::path& filepath);

        std::filesystem::path m_CurrentFilePath;
        std::string m_SourceCode;
        std::string m_CompileOutput;
        bool m_Dirty = false;
        bool m_FileLoaded = false;
    };
} // namespace OloEngine

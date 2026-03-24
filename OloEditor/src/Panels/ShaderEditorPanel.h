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

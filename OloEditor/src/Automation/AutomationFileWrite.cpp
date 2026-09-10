#include "OloEnginePCH.h"
#include "Automation/AutomationFileWrite.h"

#include "OloEngine/Core/UUID.h"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifdef OLO_PLATFORM_WINDOWS
#include "Platform/Windows/WindowsHWrapper.h"
#endif

namespace OloEngine::Automation
{
    FileContents ReadFileContents(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("Cannot inspect file", path, error);
        if (!std::filesystem::exists(status))
            return std::nullopt;
        if (!std::filesystem::is_regular_file(status))
            throw std::runtime_error("Not a regular file: " + path.string());
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Cannot read file: " + path.string());
        std::string bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        if (input.bad())
            throw std::runtime_error("Failed while reading file: " + path.string());
        return bytes;
    }

    void RequireFileContents(const std::filesystem::path& path, const FileContents& expected)
    {
        if (ReadFileContents(path) != expected)
            throw std::runtime_error("File changed outside this operation; refusing to overwrite: " + path.string());
    }

    void ReplaceFileContents(const std::filesystem::path& path, const FileContents& expected,
                             const FileContents& replacement)
    {
        RequireFileContents(path, expected);
        if (!replacement)
        {
            if (expected && !std::filesystem::remove(path))
                throw std::runtime_error("Cannot remove file: " + path.string());
            return;
        }

        auto temporary = path;
        temporary += ".automation-" + std::to_string(static_cast<u64>(UUID())) + ".tmp";
        if (std::filesystem::exists(temporary))
            throw std::runtime_error("Temporary file already exists: " + temporary.string());
        bool temporaryOwned = false;
        try
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("Cannot create temporary file: " + temporary.string());
            temporaryOwned = true;
            output.write(replacement->data(), static_cast<std::streamsize>(replacement->size()));
            output.flush();
            if (!output)
                throw std::runtime_error("Cannot write file: " + path.string());
            output.close();
            if (!output)
                throw std::runtime_error("Cannot close file: " + path.string());
            // Re-check immediately before the swap: the window between the first
            // guard and here is small but real, and this is the last moment at
            // which refusing still costs nothing.
            RequireFileContents(path, expected);
#ifdef OLO_PLATFORM_WINDOWS
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot replace file");
#else
            std::filesystem::rename(temporary, path);
#endif
        }
        catch (...)
        {
            if (temporaryOwned)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            throw;
        }
    }
} // namespace OloEngine::Automation

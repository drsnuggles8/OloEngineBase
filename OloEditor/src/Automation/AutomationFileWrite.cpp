#include "OloEnginePCH.h"
#include "Automation/AutomationFileWrite.h"

#include "OloEngine/Core/FileSystem.h"

#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

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

        // The re-check runs immediately before the swap: the window between the
        // first guard and there is small but real, and it is the last moment at
        // which refusing still costs nothing. Its exception is carried out of the
        // callback and rethrown here, outside any handler: under clang-cl + ASan a
        // throw executed lexically inside a catch handler faults in
        // __CxxFrameHandler3 (build-trees-and-windows-asan.md §4b, issue #1193).
        std::exception_ptr guardFailure;
        const auto recheckGuard = [&path, &expected, &guardFailure](const std::filesystem::path&)
        {
            try
            {
                RequireFileContents(path, expected);
                return true;
            }
            catch (...)
            {
                guardFailure = std::current_exception();
                return false;
            }
        };
        std::string writeError;
        const bool written = FileSystem::WriteFileAtomically(path, *replacement,
                                                             { .Durable = true, .BeforeReplace = recheckGuard },
                                                             &writeError);
        if (guardFailure)
            std::rethrow_exception(guardFailure);
        if (!written)
            throw std::runtime_error("Cannot write file: " + path.string() + ": " + writeError);
    }
} // namespace OloEngine::Automation

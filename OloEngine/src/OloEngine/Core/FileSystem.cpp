#include "OloEnginePCH.h"
#include "OloEngine/Core/FileSystem.h"

#include "OloEngine/Core/UUID.h"

#include <cerrno>
#include <format>
#include <fstream>
#include <system_error>

#ifndef OLO_PLATFORM_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

namespace OloEngine
{
    namespace
    {
        // The reason a just-failed stream open reported, or a fallback when the
        // C runtime left errno clear.
        [[nodiscard]] std::string LastOpenError(int errorNumber)
        {
            if (errorNumber == 0)
                return "the C runtime gave no reason";
            return std::error_code(errorNumber, std::generic_category()).message();
        }

        // Push a closed file's bytes to the disk. The stream that wrote them
        // exposes no handle, so the file is reopened for the sync, as the shape
        // cache did before it moved onto this helper.
        [[nodiscard]] bool SyncFileToDisk(const std::filesystem::path& path, std::string& outError)
        {
#ifdef OLO_PLATFORM_WINDOWS
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                outError = std::system_category().message(static_cast<int>(::GetLastError()));
                return false;
            }
            const bool flushed = ::FlushFileBuffers(file) != 0;
            if (!flushed)
                outError = std::system_category().message(static_cast<int>(::GetLastError()));
            ::CloseHandle(file);
            return flushed;
#else
            const int fd = ::open(path.c_str(), O_WRONLY);
            if (fd == -1)
            {
                outError = std::error_code(errno, std::generic_category()).message();
                return false;
            }
            const bool synced = ::fsync(fd) == 0;
            if (!synced)
                outError = std::error_code(errno, std::generic_category()).message();
            ::close(fd);
            return synced;
#endif
        }

        // Rename `from` over `to`, replacing it. std::filesystem::rename already
        // replaces on both platforms (MoveFileExW with MOVEFILE_REPLACE_EXISTING
        // under the MSVC STL, rename(2) under libstdc++); Windows calls it
        // directly only to add MOVEFILE_WRITE_THROUGH for a durable write.
        [[nodiscard]] std::error_code ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                                  [[maybe_unused]] bool durable)
        {
#ifdef OLO_PLATFORM_WINDOWS
            DWORD flags = MOVEFILE_REPLACE_EXISTING;
            if (durable)
                flags |= MOVEFILE_WRITE_THROUGH;
            if (!::MoveFileExW(from.c_str(), to.c_str(), flags))
                return { static_cast<int>(::GetLastError()), std::system_category() };
            return {};
#else
            // rename(2) has no write-through flag; the data sync before it is the durable part.
            std::error_code ec;
            std::filesystem::rename(from, to, ec);
            return ec;
#endif
        }

        // Removes the temporary on every exit that did not rename it into
        // place, including an exception thrown by a writer or a veto callback.
        class TemporaryFile
        {
          public:
            explicit TemporaryFile(std::filesystem::path path)
                : m_Path(std::move(path))
            {
            }
            TemporaryFile(const TemporaryFile&) = delete;
            TemporaryFile& operator=(const TemporaryFile&) = delete;
            ~TemporaryFile()
            {
                if (!m_Released)
                {
                    std::error_code ignored;
                    std::filesystem::remove(m_Path, ignored);
                }
            }

            [[nodiscard]] const std::filesystem::path& Path() const
            {
                return m_Path;
            }
            void Release()
            {
                m_Released = true;
            }

          private:
            std::filesystem::path m_Path;
            bool m_Released = false;
        };
    } // namespace

    bool FileSystem::WriteFileAtomically(const std::filesystem::path& path, std::string_view bytes,
                                         const AtomicWriteOptions& options, std::string* outError)
    {
        return WriteFileAtomically(
            path,
            [bytes](std::ostream& out)
            {
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                return true;
            },
            options, outError);
    }

    bool FileSystem::WriteFileAtomically(const std::filesystem::path& path, std::span<const u8> bytes,
                                         const AtomicWriteOptions& options, std::string* outError)
    {
        return WriteFileAtomically(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                   options, outError);
    }

    bool FileSystem::WriteFileAtomically(const std::filesystem::path& path,
                                         const std::function<bool(std::ostream&)>& write,
                                         const AtomicWriteOptions& options, std::string* outError)
    {
        OLO_PROFILE_FUNCTION();

        const auto fail = [&path, outError](std::string reason)
        {
            OLO_CORE_ERROR("FileSystem: '{}' was not written: {}", path.string(), reason);
            if (outError)
                *outError = std::move(reason);
            return false;
        };

        std::error_code ec;
        std::filesystem::path target = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            target = path;

        if (options.CreateParentDirectories)
        {
            if (const auto parent = target.parent_path(); !parent.empty())
            {
                std::filesystem::create_directories(parent, ec);
                if (ec)
                    return fail(std::format("cannot create the directory '{}': {}", parent.string(), ec.message()));
            }
        }

        std::filesystem::path temporaryPath = target;
        temporaryPath += std::format(".{:016x}.tmp", static_cast<u64>(UUID()));
        TemporaryFile temporary(temporaryPath);
        {
            std::ios::openmode mode = std::ios::out | std::ios::trunc;
            if (!options.TextMode)
                mode |= std::ios::binary;
            errno = 0;
            std::ofstream out(temporary.Path(), mode);
            if (!out.is_open())
                return fail(std::format("cannot create '{}': {}", temporary.Path().string(), LastOpenError(errno)));
            const bool written = write(out);
            out.flush();
            // Closed before the state is read: a full disk reports at the final
            // flush, which a check while the stream is still open does not see.
            out.close();
            if (!written)
            {
                // The writer's own decision, which it has reported; not logged again.
                if (outError)
                    *outError = "the writer abandoned it";
                return false;
            }
            if (out.fail())
                return fail(std::format("writing '{}' failed", temporary.Path().string()));
        }

        if (options.Durable)
        {
            std::string syncError;
            if (!SyncFileToDisk(temporary.Path(), syncError))
                return fail(std::format("cannot flush '{}' to disk: {}", temporary.Path().string(), syncError));
        }

        if (options.BeforeReplace && !options.BeforeReplace(temporary.Path()))
            return false;

        if (const std::error_code renameError = ReplaceFile(temporary.Path(), target, options.Durable))
            return fail(std::format("cannot replace it with '{}': {}", temporary.Path().string(), renameError.message()));
        temporary.Release();
        return true;
    }

    Buffer FileSystem::ReadFileBinary(const std::filesystem::path& filepath)
    {
        std::ifstream stream(filepath, std::ios::binary | std::ios::ate);

        if (!stream)
        {
            return {};
        }

        std::streampos end = stream.tellg();
        stream.seekg(0, std::ios::beg);
        auto size = static_cast<u64>(end - stream.tellg());

        if (size == 0)
        {
            return {};
        }

        Buffer buffer(size);
        stream.read(buffer.As<char>(), static_cast<std::streamsize>(size));
        stream.close();
        return buffer;
    }

    std::string FileSystem::ReadFileText(const std::filesystem::path& filepath)
    {
        std::string result;
        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (!in)
        {
            OLO_CORE_ERROR("Could not open file '{0}'", filepath.string());
            return result;
        }

        in.seekg(0, std::ios::end);
        if (const auto size = in.tellg(); size != std::streampos(-1))
        {
            result.resize(static_cast<sizet>(size));
            in.seekg(0, std::ios::beg);
            in.read(result.data(), size);
        }
        else
        {
            OLO_CORE_ERROR("Could not read file '{0}'", filepath.string());
        }
        return result;
    }

    bool FileSystem::IsNewer(const std::filesystem::path& pathA, const std::filesystem::path& pathB)
    {
        std::error_code ec;
        if (!std::filesystem::exists(pathA, ec) || !std::filesystem::exists(pathB, ec))
        {
            return false;
        }

        auto const timeA = std::filesystem::last_write_time(pathA, ec);
        if (ec)
        {
            return false;
        }

        auto const timeB = std::filesystem::last_write_time(pathB, ec);
        if (ec)
        {
            return false;
        }

        return timeA > timeB;
    }

    std::filesystem::path FileSystem::GetExecutableDirectory()
    {
#if defined(OLO_PLATFORM_WINDOWS)
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0)
            {
                return {};
            }
            if (written < buffer.size())
            {
                buffer.resize(written);
                break;
            }
            // Truncated (returned == capacity): grow and retry.
            buffer.resize(buffer.size() * 2);
        }
        return std::filesystem::path(buffer).parent_path();
#elif defined(OLO_PLATFORM_LINUX)
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec)
        {
            return {};
        }
        return exe.parent_path();
#else
        return {};
#endif
    }

} // namespace OloEngine

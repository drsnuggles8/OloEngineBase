#include "OloEnginePCH.h"
#include "FileStream.h"
#include <stdexcept>
#include <cstring>

namespace OloEngine
{
    //==============================================================================
    /// FileStreamWriter
    FileStreamWriter::FileStreamWriter(const std::filesystem::path& path)
        : m_Path(path), m_Stream(path, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc)
    {
        if (!m_Stream.is_open() || m_Stream.fail())
        {
            OLO_CORE_ERROR("Failed to open file for writing: {} (error: {})", path.string(), std::strerror(errno));
            throw std::runtime_error("Failed to open file for writing: " + path.string());
        }
    }

    bool FileStreamWriter::WriteData(const char* data, sizet size)
    {
        m_Stream.write(data, size);

        // Check for immediate write failure
        if (m_Stream.fail())
        {
            return false;
        }

        // Flush to ensure data is committed to disk
        m_Stream.flush();

        // Final state check after flush
        return m_Stream.good();
    }

    //==============================================================================
    /// FileStreamReader
    FileStreamReader::FileStreamReader(const std::filesystem::path& path)
        : m_Path(path), m_Stream(path, std::ifstream::in | std::ifstream::binary)
    {
        if (!m_Stream.is_open() || m_Stream.fail())
        {
            OLO_CORE_ERROR("Failed to open file for reading: {} (error: {})", path.string(), std::strerror(errno));
            throw std::runtime_error("Failed to open file for reading: " + path.string());
        }
    }

    std::optional<u64> FileStreamReader::GetStreamSize()
    {
        const auto position = m_Stream.tellg();
        if (position == std::streampos(-1))
            return std::nullopt;
        m_Stream.seekg(0, std::ios::end);
        const auto size = m_Stream.tellg();
        m_Stream.seekg(position);
        if (size == std::streampos(-1) || !m_Stream)
            return std::nullopt;
        return static_cast<u64>(size);
    }

    bool FileStreamReader::ReadData(char* destination, sizet size)
    {
        m_Stream.read(destination, size);

        // Check if the full amount was read
        if (m_Stream.gcount() != static_cast<std::streamsize>(size))
        {
            return false; // Short read
        }

        // Check for stream errors (but allow EOF after successful full read)
        if (m_Stream.fail() && !m_Stream.eof())
        {
            return false; // I/O error occurred
        }

        return true; // Full read successful
    }

} // namespace OloEngine

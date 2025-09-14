#pragma once

#include "StreamWriter.h"
#include "StreamReader.h"
#include "OloEngine/Core/Buffer.h"

#include <filesystem>
#include <fstream>

namespace OloEngine
{
    //==============================================================================
    /// FileStreamWriter
    class FileStreamWriter : public StreamWriter
    {
    public:
        explicit FileStreamWriter(const std::filesystem::path& path);
        FileStreamWriter(const FileStreamWriter&) = delete;
        virtual ~FileStreamWriter() noexcept = default;

        [[nodiscard]] bool IsStreamGood() const final { return m_Stream.good(); }
        [[nodiscard]] u64 GetStreamPosition() final
        {
            const auto pos = m_Stream.tellp();
            return pos == std::streampos(-1) ? 0ull : static_cast<u64>(pos);
        }
        void SetStreamPosition(u64 position) final
        {
            m_Stream.seekp(static_cast<std::streampos>(position));
        }
        bool WriteData(const char* data, sizet size) final;

    private:
        std::filesystem::path m_Path;
        std::ofstream m_Stream;
    };

    //==============================================================================
    /// FileStreamReader
    class FileStreamReader : public StreamReader
    {
    public:
        explicit FileStreamReader(const std::filesystem::path& path);
        FileStreamReader(const FileStreamReader&) = delete;
        ~FileStreamReader() noexcept override = default;

        [[nodiscard]] const std::filesystem::path& GetFilePath() const { return m_Path; }

        [[nodiscard]] bool IsStreamGood() const final { return m_Stream.good(); }
        [[nodiscard]] u64 GetStreamPosition() override
        {
            const auto pos = m_Stream.tellg();
            return pos == std::streampos(-1) ? 0ull : static_cast<u64>(pos);
        }
        void SetStreamPosition(u64 position) override
        {
            m_Stream.seekg(static_cast<std::streampos>(position));
        }
        bool ReadData(char* destination, sizet size) override;

    private:
        std::filesystem::path m_Path;
        std::ifstream m_Stream;
    };

} // namespace OloEngine

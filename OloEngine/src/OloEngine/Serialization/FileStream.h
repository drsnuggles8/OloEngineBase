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
        FileStreamWriter(const std::filesystem::path& path);
        FileStreamWriter(const FileStreamWriter&) = delete;
        virtual ~FileStreamWriter();

        bool IsStreamGood() const final { return m_Stream.good(); }
        u64 GetStreamPosition() final { return m_Stream.tellp(); }
        void SetStreamPosition(u64 position) final { m_Stream.seekp(position); }
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
        FileStreamReader(const std::filesystem::path& path);
        FileStreamReader(const FileStreamReader&) = delete;
        ~FileStreamReader();

        const std::filesystem::path& GetFilePath() const { return m_Path; }

        bool IsStreamGood() const final { return m_Stream.good(); }
        u64 GetStreamPosition() override { return m_Stream.tellg(); }
        void SetStreamPosition(u64 position) override { m_Stream.seekg(position); }
        bool ReadData(char* destination, sizet size) override;

    private:
        std::filesystem::path m_Path;
        std::ifstream m_Stream;
    };

} // namespace OloEngine

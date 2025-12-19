#pragma once

#include "OloEngine/Core/Buffer.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <string>

namespace OloEngine
{
    class StreamWriter
    {
      public:
        virtual ~StreamWriter() = default;

        virtual bool IsStreamGood() const = 0;
        virtual u64 GetStreamPosition() = 0;
        virtual void SetStreamPosition(u64 position) = 0;
        virtual bool WriteData(const char* data, sizet size) = 0;

        operator bool() const
        {
            return IsStreamGood();
        }

        void WriteBuffer(Buffer buffer, bool writeSize = true);
        void WriteZero(u64 size);

        /// @brief Writes a string with u64 length prefix in little-endian format
        /// @note Compatible with StreamReader::ReadString which expects u64 length
        void WriteString(const std::string& string);

        template<typename T>
        void WriteRaw(const T& type)
        {
            bool success = WriteData(reinterpret_cast<const char*>(&type), sizeof(T));
            OLO_CORE_ASSERT(success);
        }

        template<typename T>
        void WriteObject(const T& obj)
        {
            static_assert(std::is_class_v<T>, "WriteObject requires a class type");
            // Note: Compile-time validation of Serialize method existence would require concepts (C++20)
            // For C++17 compatibility, we rely on template instantiation errors
            T::Serialize(this, obj);
        }

        template<typename Key, typename Value>
        void WriteMap(const std::map<Key, Value>& map, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>((u32)map.size());

            for (const auto& [key, value] : map)
            {
                if constexpr (std::is_trivially_copyable_v<Key>)
                    WriteRaw<Key>(key);
                else
                    WriteObject<Key>(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    WriteRaw<Value>(value);
                else
                    WriteObject<Value>(value);
            }
        }

        template<typename Key, typename Value>
        void WriteMap(const std::unordered_map<Key, Value>& map, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>((u32)map.size());

            for (const auto& [key, value] : map)
            {
                if constexpr (std::is_trivially_copyable_v<Key>)
                    WriteRaw<Key>(key);
                else
                    WriteObject<Key>(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    WriteRaw<Value>(value);
                else
                    WriteObject<Value>(value);
            }
        }

        template<typename Value>
        void WriteMap(const std::unordered_map<std::string, Value>& map, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>((u32)map.size());

            for (const auto& [key, value] : map)
            {
                WriteString(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    WriteRaw<Value>(value);
                else
                    WriteObject<Value>(value);
            }
        }

        template<typename T>
        void WriteArray(const std::vector<T>& array, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>((u32)array.size());

            for (const auto& element : array)
            {
                if constexpr (std::is_trivially_copyable_v<T>)
                    WriteRaw<T>(element);
                else
                    WriteObject<T>(element);
            }
        }
    };

    template<>
    inline void StreamWriter::WriteArray(const std::vector<std::string>& array, bool writeSize)
    {
        if (writeSize)
            WriteRaw<u32>((u32)array.size());

        for (const auto& element : array)
            WriteString(element);
    }

} // namespace OloEngine

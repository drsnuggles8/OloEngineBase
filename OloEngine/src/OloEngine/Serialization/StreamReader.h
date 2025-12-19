#pragma once

#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Asset/Asset.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <span>

namespace OloEngine
{
    class StreamReader
    {
      public:
        virtual ~StreamReader() = default;

        virtual bool IsStreamGood() const = 0;
        virtual u64 GetStreamPosition() = 0;
        virtual void SetStreamPosition(u64 position) = 0;
        virtual bool ReadData(char* destination, sizet size) = 0;

        /// @brief Safe ReadData overload using std::span<std::byte> for type safety
        /// @param destination Span of bytes to read data into
        /// @return true if the full amount was read successfully
        bool ReadData(std::span<std::byte> destination);

        operator bool() const
        {
            return IsStreamGood();
        }

        void ReadBuffer(Buffer& buffer, u32 size = 0);

        /// @brief Reads a string with u64 length prefix in little-endian format
        /// @note Compatible with StreamWriter::WriteString which writes u64 length
        void ReadString(std::string& string);

        template<typename T>
        void ReadRaw(T& type)
        {
            bool success = ReadData(reinterpret_cast<char*>(&type), sizeof(T));
            OLO_CORE_ASSERT(success);
        }

        template<typename T>
        void ReadObject(T& obj)
        {
            static_assert(std::is_class_v<T>, "ReadObject requires a class type");
            // Note: Template can now be used with any class providing a static Deserialize method
            // For C++17 compatibility, we rely on template instantiation errors for method validation
            T::Deserialize(this, obj);
        }

        template<typename Key, typename Value>
        void ReadMap(std::map<Key, Value>& map, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            for (u32 i = 0; i < size; i++)
            {
                Key key;
                if constexpr (std::is_trivially_copyable_v<Key>)
                    ReadRaw<Key>(key);
                else
                    ReadObject<Key>(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    ReadRaw<Value>(map[key]);
                else
                    ReadObject<Value>(map[key]);
            }
        }

        template<typename Key, typename Value>
        void ReadMap(std::unordered_map<Key, Value>& map, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            for (u32 i = 0; i < size; i++)
            {
                Key key;
                if constexpr (std::is_trivially_copyable_v<Key>)
                    ReadRaw<Key>(key);
                else
                    ReadObject<Key>(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    ReadRaw<Value>(map[key]);
                else
                    ReadObject<Value>(map[key]);
            }
        }

        template<typename Value>
        void ReadMap(std::unordered_map<std::string, Value>& map, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            for (u32 i = 0; i < size; i++)
            {
                std::string key;
                ReadString(key);

                if constexpr (std::is_trivially_copyable_v<Value>)
                    ReadRaw<Value>(map[key]);
                else
                    ReadObject<Value>(map[key]);
            }
        }

        template<typename T>
        void ReadArray(std::vector<T>& array, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            array.resize(size);

            for (u32 i = 0; i < size; i++)
            {
                if constexpr (std::is_trivially_copyable_v<T>)
                    ReadRaw<T>(array[i]);
                else
                    ReadObject<T>(array[i]);
            }
        }
    };

    template<>
    inline void StreamReader::ReadArray(std::vector<std::string>& array, u32 size)
    {
        if (size == 0)
            ReadRaw<u32>(size);

        array.resize(size);

        for (u32 i = 0; i < size; i++)
            ReadString(array[i]);
    }

} // namespace OloEngine

#pragma once

#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Asset/Asset.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <string>

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

        operator bool() const { return IsStreamGood(); }

        void ReadBuffer(Buffer& buffer, u32 size = 0);
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
            static_assert(std::is_same_v<AssetHandle, T>, "ReadObject can only be used with AssetHandle type");
            // Note: Compile-time validation of Deserialize method existence would require concepts (C++20)
            // For C++17 compatibility, we rely on template instantiation errors
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

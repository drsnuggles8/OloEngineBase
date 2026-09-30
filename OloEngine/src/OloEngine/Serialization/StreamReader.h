#pragma once

#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Asset/Asset.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <span>
#include <type_traits>

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

        /// @brief Reads any associative container (std::map / std::unordered_map).
        /// @note Each key and value is read via ReadElement, mirroring StreamWriter::WriteMap:
        ///       std::string keys/values come from ReadString, trivially-copyable elements are
        ///       raw, the rest use their static Deserialize. A size of 0 means "read the u32
        ///       count first"; pass a non-zero size to skip the prefix.
        template<typename Map>
        void ReadMap(Map& map, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            using KeyType = typename Map::key_type;
            for (u32 i = 0; i < size; ++i)
            {
                KeyType key;
                ReadElement(key);
                ReadElement(map[key]);
            }
        }

        /// @brief Reads a vector. Each element is read via ReadElement (see ReadMap).
        /// @note A size of 0 means "read the u32 count first"; pass a non-zero size to skip it.
        template<typename T>
        void ReadArray(std::vector<T>& array, u32 size = 0)
        {
            if (size == 0)
                ReadRaw<u32>(size);

            array.resize(size);

            for (u32 i = 0; i < size; ++i)
                ReadElement(array[i]);
        }

      private:
        /// @brief Reads a single container element with the right primitive: std::string →
        ///        ReadString (length-prefixed), trivially-copyable → ReadRaw (raw bytes),
        ///        otherwise ReadObject (static Deserialize).
        template<typename T>
        void ReadElement(T& element)
        {
            if constexpr (std::is_same_v<T, std::string>)
                ReadString(element);
            else if constexpr (std::is_trivially_copyable_v<T>)
                ReadRaw<T>(element);
            else
                ReadObject<T>(element);
        }
    };

} // namespace OloEngine

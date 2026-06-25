#pragma once

#include "OloEngine/Core/Buffer.h"

#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <type_traits>

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

        /// @brief Writes any associative container (std::map / std::unordered_map).
        /// @note Each key and value is written via WriteElement, so std::string keys/values go
        ///       through WriteString (length-prefixed) while trivially-copyable elements are raw
        ///       and the rest use their static Serialize. Wire format: optional u32 count, then
        ///       count × { key, value }.
        template<typename Map>
        void WriteMap(const Map& map, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>(static_cast<u32>(map.size()));

            for (const auto& [key, value] : map)
            {
                WriteElement(key);
                WriteElement(value);
            }
        }

        /// @brief Writes a vector. Each element is written via WriteElement (see WriteMap).
        /// @note Wire format: optional u32 count, then count × element.
        template<typename T>
        void WriteArray(const std::vector<T>& array, bool writeSize = true)
        {
            if (writeSize)
                WriteRaw<u32>(static_cast<u32>(array.size()));

            for (const auto& element : array)
                WriteElement(element);
        }

      private:
        /// @brief Writes a single container element with the right primitive: std::string →
        ///        WriteString (length-prefixed), trivially-copyable → WriteRaw (raw bytes),
        ///        otherwise WriteObject (static Serialize).
        template<typename T>
        void WriteElement(const T& element)
        {
            if constexpr (std::is_same_v<T, std::string>)
                WriteString(element);
            else if constexpr (std::is_trivially_copyable_v<T>)
                WriteRaw<T>(element);
            else
                WriteObject<T>(element);
        }
    };

} // namespace OloEngine

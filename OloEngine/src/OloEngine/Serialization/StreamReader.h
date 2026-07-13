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

        /// @brief Format version of the archive being read (FArchive's ArArchiveVersion
        /// equivalent for the fixed-order asset-pack streams).
        ///
        /// A pack stream is NOT self-describing at the field level: every ReadRaw pulls
        /// the next N bytes unconditionally, so a field appended in a later format
        /// version MUST be gated on the version that introduced it or reading an older
        /// pack silently desyncs every subsequent field (docs/agent-rules/
        /// binary-format-versioning.md, issue #454). AssetPack::GetAssetStreamReader
        /// stamps every reader it hands out with the pack's recorded Header.Version, so
        /// a read site can gate with `stream.GetArchiveVersion() >= kIntroducedIn`.
        /// Defaults to AssetPackFile::Version so a stream that is not pack-backed (unit
        /// tests, ad-hoc readers) sees the current layout.
        [[nodiscard]] u32 GetArchiveVersion() const
        {
            return m_ArchiveVersion;
        }

        void SetArchiveVersion(u32 version)
        {
            m_ArchiveVersion = version;
        }

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

        // "Newest" by default, so every `>= kIntroducedIn` gate passes and a stream that
        // is not asset-pack-backed reads the current layout. AssetPack overwrites this
        // with the pack's real Header.Version, which is the only case where an OLDER
        // layout can actually appear on disk.
        u32 m_ArchiveVersion = ~0u;
    };

} // namespace OloEngine

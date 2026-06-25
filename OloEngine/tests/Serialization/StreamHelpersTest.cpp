// OLO_TEST_LAYER: unit
//
// Round-trip + wire-format coverage for the StreamWriter / StreamReader container
// helpers (WriteMap/ReadMap, WriteArray/ReadArray) after they were collapsed onto a
// single string-aware WriteElement/ReadElement helper. The byte-level assertions pin
// the on-disk format so the dedup refactor cannot silently change it: trivially-copyable
// elements stay raw, std::string keys/values stay length-prefixed, and class elements
// route through the static Serialize/Deserialize contract.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Serialization/StreamWriter.h"
#include "OloEngine/Serialization/StreamReader.h"

#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace OloEngine;

namespace
{
    // Minimal in-memory StreamWriter/StreamReader pair backed by a byte vector, so the
    // container helpers can be round-tripped without touching the filesystem.
    class MemoryStreamWriter final : public StreamWriter
    {
      public:
        explicit MemoryStreamWriter(std::vector<char>& buffer) : m_Buffer(buffer) {}

        bool IsStreamGood() const override
        {
            return true;
        }
        u64 GetStreamPosition() override
        {
            return static_cast<u64>(m_Position);
        }
        void SetStreamPosition(u64 position) override
        {
            m_Position = static_cast<sizet>(position);
        }

        bool WriteData(const char* data, sizet size) override
        {
            if (m_Position + size > m_Buffer.size())
                m_Buffer.resize(m_Position + size);
            if (size > 0)
                std::memcpy(m_Buffer.data() + m_Position, data, size);
            m_Position += size;
            return true;
        }

      private:
        std::vector<char>& m_Buffer;
        sizet m_Position = 0;
    };

    class MemoryStreamReader final : public StreamReader
    {
      public:
        explicit MemoryStreamReader(const std::vector<char>& buffer) : m_Buffer(buffer) {}

        bool IsStreamGood() const override
        {
            return m_Position <= m_Buffer.size();
        }
        u64 GetStreamPosition() override
        {
            return static_cast<u64>(m_Position);
        }
        void SetStreamPosition(u64 position) override
        {
            m_Position = static_cast<sizet>(position);
        }

        bool ReadData(char* destination, sizet size) override
        {
            if (m_Position + size > m_Buffer.size())
                return false;
            if (size > 0)
                std::memcpy(destination, m_Buffer.data() + m_Position, size);
            m_Position += size;
            return true;
        }

      private:
        const std::vector<char>& m_Buffer;
        sizet m_Position = 0;
    };

    // Non-trivially-copyable element type exercising the WriteObject/ReadObject branch via the
    // static Serialize/Deserialize contract the helpers expect.
    struct Widget
    {
        std::string Name;
        u32 Id = 0;

        bool operator==(const Widget& other) const
        {
            return Name == other.Name && Id == other.Id;
        }

        static void Serialize(StreamWriter* writer, const Widget& w)
        {
            writer->WriteString(w.Name);
            writer->WriteRaw<u32>(w.Id);
        }

        static void Deserialize(StreamReader* reader, Widget& w)
        {
            reader->ReadString(w.Name);
            reader->ReadRaw<u32>(w.Id);
        }
    };

    // Helpers that build an expected byte layout independently of the stream helpers, so the
    // wire-format assertions encode the format spec rather than mirroring the implementation.
    template<typename T>
    void AppendRaw(std::vector<char>& buffer, const T& value)
    {
        const char* bytes = reinterpret_cast<const char*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
    }

    void AppendString(std::vector<char>& buffer, const std::string& s)
    {
        AppendRaw(buffer, static_cast<u64>(s.size()));
        buffer.insert(buffer.end(), s.begin(), s.end());
    }
} // namespace

// ============================================================================
// Array round-trips — trivially-copyable, std::string (folded specialization),
// and the class/WriteObject branch.
// ============================================================================

TEST(StreamHelpers, ArrayOfTrivialRoundtrips)
{
    std::vector<u32> original{ 7, 8, 9, 0, 4242 };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    std::vector<u32> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadArray(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, ArrayOfStringsRoundtrips)
{
    std::vector<std::string> original{ "hello", "world", "", "a longer string with spaces" };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    std::vector<std::string> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadArray(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, ArrayOfObjectsRoundtrips)
{
    std::vector<Widget> original{ { "alpha", 1 }, { "", 0 }, { "gamma", 4242 } };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    std::vector<Widget> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadArray(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, EmptyArrayRoundtrips)
{
    std::vector<u32> original;

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    std::vector<u32> loaded{ 1, 2, 3 }; // pre-filled to verify it is cleared
    MemoryStreamReader reader(buffer);
    reader.ReadArray(loaded);

    EXPECT_TRUE(loaded.empty());
}

// ============================================================================
// Map round-trips — every key/value combination the helpers must support.
// ============================================================================

TEST(StreamHelpers, MapTrivialKeyTrivialValueRoundtrips)
{
    std::map<u32, f32> original{ { 1, 1.5f }, { 2, -2.75f }, { 99, 0.0f } };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    std::map<u32, f32> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadMap(loaded);

    ASSERT_EQ(loaded.size(), original.size());
    for (const auto& [key, value] : original)
    {
        ASSERT_TRUE(loaded.contains(key));
        EXPECT_FLOAT_EQ(loaded[key], value);
    }
}

TEST(StreamHelpers, UnorderedMapStringKeyRoundtrips)
{
    std::unordered_map<std::string, u32> original{ { "speed", 10 }, { "health", 100 }, { "", 7 } };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    std::unordered_map<std::string, u32> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadMap(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, MapStringKeyObjectValueRoundtrips)
{
    std::map<std::string, Widget> original{
        { "first", { "alpha", 1 } },
        { "second", { "beta", 2 } },
    };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    std::map<std::string, Widget> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadMap(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, MapTrivialKeyStringValueRoundtrips)
{
    std::unordered_map<u32, std::string> original{ { 1, "one" }, { 2, "two" }, { 3, "" } };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    std::unordered_map<u32, std::string> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadMap(loaded);

    EXPECT_EQ(loaded, original);
}

TEST(StreamHelpers, EmptyMapRoundtrips)
{
    std::unordered_map<std::string, u32> original;

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    // Unlike ReadArray (which resize()s and so clears), ReadMap inserts into the destination
    // and never clears it — a count of 0 therefore performs no inserts. Read into a fresh map.
    std::unordered_map<std::string, u32> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadMap(loaded);

    EXPECT_TRUE(loaded.empty());
}

// ============================================================================
// writeSize=false / explicit-size path — mirrors the real callers (SoundGraph
// prototype, font ranges) that write/read the count out of band.
// ============================================================================

TEST(StreamHelpers, ArrayWithoutSizePrefixRoundtrips)
{
    std::vector<u32> original{ 11, 22, 33 };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original, /*writeSize=*/false); // no count prefix

    std::vector<u32> loaded;
    MemoryStreamReader reader(buffer);
    reader.ReadArray(loaded, static_cast<u32>(original.size())); // caller supplies the count

    EXPECT_EQ(loaded, original);
}

// ============================================================================
// Wire-format pins — the dedup must not change the bytes on disk. Expected
// buffers are built independently via AppendRaw/AppendString.
// ============================================================================

TEST(StreamHelpers, TrivialArrayWireFormatIsRawCountThenElements)
{
    std::vector<u32> original{ 7, 8 };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    std::vector<char> expected;
    AppendRaw(expected, static_cast<u32>(original.size()));
    for (auto v : original)
        AppendRaw(expected, v);

    EXPECT_EQ(buffer, expected);
}

TEST(StreamHelpers, StringArrayWireFormatIsLengthPrefixed)
{
    std::vector<std::string> original{ "ab", "" };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteArray(original);

    // Strings must stay length-prefixed (u64 length + bytes), never raw — this is the
    // behaviour the folded std::vector<std::string> specialization used to provide.
    std::vector<char> expected;
    AppendRaw(expected, static_cast<u32>(original.size()));
    for (const auto& s : original)
        AppendString(expected, s);

    EXPECT_EQ(buffer, expected);
}

TEST(StreamHelpers, StringKeyMapWireFormatMatchesManualStringThenValue)
{
    // A single-entry map writes: u32 count, then (u64 keyLen + key bytes), then raw value.
    std::unordered_map<std::string, u32> original{ { "key", 42 } };

    std::vector<char> buffer;
    MemoryStreamWriter writer(buffer);
    writer.WriteMap(original);

    std::vector<char> expected;
    AppendRaw(expected, static_cast<u32>(original.size()));
    AppendString(expected, "key");
    AppendRaw(expected, static_cast<u32>(42));

    EXPECT_EQ(buffer, expected);
}

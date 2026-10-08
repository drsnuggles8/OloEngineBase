#include "OloEnginePCH.h"
#include "FoliageStreamingPayload.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"

#include <assimp/Importer.hpp>
#include <assimp/DefaultIOSystem.h>
#include <assimp/IOStream.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <fstream>
#include <memory>
#include <chrono>

namespace OloEngine
{
    namespace
    {
        // Instrument the reads Assimp actually performs, including material
        // sidecars, independently from parsing and mesh preparation time.
        class FMeasuredAssimpStream final : public Assimp::IOStream
        {
          public:
            FMeasuredAssimpStream(Assimp::IOStream* stream, Assimp::DefaultIOSystem& system, FRepresentationIOStats& stats)
                : m_Stream(stream), m_System(system), m_Stats(stats) {}
            ~FMeasuredAssimpStream() override
            {
                m_System.Close(m_Stream);
            }
            size_t Read(void* buffer, size_t size, size_t count) override
            {
                const auto start = std::chrono::steady_clock::now();
                const auto read = m_Stream->Read(buffer, size, count);
                m_Stats.ReadMicroseconds += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                m_Stats.ReadBytes += static_cast<u64>(read) * size;
                return read;
            }
            size_t Write(const void* buffer, size_t size, size_t count) override
            {
                return m_Stream->Write(buffer, size, count);
            }
            aiReturn Seek(size_t offset, aiOrigin origin) override
            {
                return m_Stream->Seek(offset, origin);
            }
            size_t Tell() const override
            {
                return m_Stream->Tell();
            }
            size_t FileSize() const override
            {
                return m_Stream->FileSize();
            }
            void Flush() override
            {
                m_Stream->Flush();
            }

          private:
            Assimp::IOStream* m_Stream;
            Assimp::DefaultIOSystem& m_System;
            FRepresentationIOStats& m_Stats;
        };
        class FMeasuredAssimpIO final : public Assimp::IOSystem
        {
          public:
            explicit FMeasuredAssimpIO(FRepresentationIOStats& stats) : m_Stats(stats) {}
            bool Exists(const char* path) const override
            {
                return m_System.Exists(path);
            }
            char getOsSeparator() const override
            {
                return m_System.getOsSeparator();
            }
            Assimp::IOStream* Open(const char* path, const char* mode = "rb") override
            {
                auto* stream = m_System.Open(path, mode);
                return stream ? new FMeasuredAssimpStream(stream, m_System, m_Stats) : nullptr;
            }
            void Close(Assimp::IOStream* stream) override
            {
                delete stream;
            }
            bool ComparePaths(const char* one, const char* two) const override
            {
                return m_System.ComparePaths(one, two);
            }

          private:
            Assimp::DefaultIOSystem m_System;
            FRepresentationIOStats& m_Stats;
        };

        bool ProbeImage(const std::filesystem::path& path, int& width, int& height, int& channels, FRepresentationIOStats& stats)
        {
            struct FSource
            {
                std::ifstream File;
                FRepresentationIOStats& Stats;
            } source{ std::ifstream(path, std::ios::binary), stats };
            if (!source.File)
                return false;
            const stbi_io_callbacks callbacks{
                [](void* context, char* buffer, int count)
                {
                    auto& input = *static_cast<FSource*>(context);
                    const auto start = std::chrono::steady_clock::now();
                    input.File.read(buffer, count);
                    const auto bytes = input.File.gcount();
                    input.Stats.ReadBytes += static_cast<u64>(bytes);
                    input.Stats.ReadMicroseconds += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                    return static_cast<int>(bytes);
                },
                [](void* context, int count)
                {
                    auto& input = *static_cast<FSource*>(context);
                    input.File.clear();
                    input.File.seekg(count, std::ios::cur);
                },
                [](void* context)
                { return static_cast<FSource*>(context)->File.eof() ? 1 : 0; }
            };
            return stbi_info_from_callbacks(&callbacks, &source, &width, &height, &channels) != 0;
        }
    } // namespace
    Ref<FFoliageTextureStaging> FFoliageTextureStaging::Reserve(u64 bytes)
    {
        const auto ticket = RepresentationStreaming::Get().ReserveStaging(FAssetByteSize::Estimate(bytes));
        return ticket ? Ref<FFoliageTextureStaging>(new FFoliageTextureStaging(bytes, *ticket)) : Ref<FFoliageTextureStaging>{};
    }

    FFoliageTextureStaging::~FFoliageTextureStaging()
    {
        RepresentationStreaming::Get().ReleaseStaging(m_Ticket);
    }

    FFoliageStreamingImage::~FFoliageStreamingImage()
    {
        // Destroy retained pixels BEFORE giving their reservation back.
        Pixels.Empty();
        Staging.Reset();
    }

    u8 FFoliageStreamingImage::AlphaAt(glm::vec2 uv) const noexcept
    {
        if (Width == 0 || Height == 0 || static_cast<u64>(Pixels.Num()) != static_cast<u64>(Width) * Height * 4u)
            return 255u;
        const f32 u = std::isfinite(uv.x) ? uv.x - std::floor(uv.x) : 0.0f;
        const f32 v = std::isfinite(uv.y) ? uv.y - std::floor(uv.y) : 0.0f;
        const u32 x = std::min(static_cast<u32>(u * static_cast<f32>(Width)), Width - 1u);
        // Pixels already have Texture2D's vertically flipped upload order.
        const u32 row = std::min(static_cast<u32>(v * static_cast<f32>(Height)), Height - 1u);
        return Pixels[static_cast<i32>((static_cast<u64>(row) * Width + x) * 4u + 3u)];
    }

    Ref<FFoliageStreamingImage> DecodeFoliageStreamingAlbedo(const FFoliageStreamingPart& part, Ref<FFoliageTextureStaging> staging)
    {
        if (part.AlbedoPath.IsEmpty() || part.AlbedoSourceBytes == 0 ||
            part.AlbedoSourceBytes > static_cast<u64>(std::numeric_limits<i32>::max()) ||
            part.AlbedoUploadBytes == 0 || part.AlbedoUploadBytes > static_cast<u64>(std::numeric_limits<i32>::max()) ||
            part.AlbedoWidth == 0 || part.AlbedoHeight == 0 ||
            static_cast<u64>(part.AlbedoWidth) * part.AlbedoHeight * 4u != part.AlbedoUploadBytes ||
            part.AlbedoDecodeBytes < part.AlbedoUploadBytes * 8u + part.AlbedoSourceBytes * 2u + 65536u)
            return {};
        if (!staging)
            staging = FFoliageTextureStaging::Reserve(part.AlbedoDecodeBytes);
        if (!staging || staging->Capacity() < part.AlbedoDecodeBytes)
            return {};
        auto image = Ref<FFoliageStreamingImage>::Create();
        image->Staging = std::move(staging);
        // A bounded immutable byte copy avoids racing a path-based decoder
        // against an image replaced since the worker measured its dimensions.
        TArray<u8> source;
        source.SetNumUninitialized(static_cast<i32>(part.AlbedoSourceBytes));
        std::ifstream file(part.AlbedoPath.ToStdString(), std::ios::binary);
        const auto readStart = std::chrono::steady_clock::now();
        file.read(reinterpret_cast<char*>(source.GetData()), static_cast<std::streamsize>(source.Num()));
        const auto readBytes = file.gcount();
        const auto nextByte = file ? file.peek() : std::char_traits<char>::eof();
        RepresentationStreaming::Get().RecordIO({ static_cast<u64>(readBytes) + (nextByte == std::char_traits<char>::eof() ? 0u : 1u),
                                                  static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - readStart).count()) });
        if (readBytes != source.Num() || nextByte != std::char_traits<char>::eof())
            return {};
        int width = 0, height = 0, channels = 0;
        if (!stbi_info_from_memory(source.GetData(), source.Num(), &width, &height, &channels) ||
            width != static_cast<int>(part.AlbedoWidth) || height != static_cast<int>(part.AlbedoHeight))
            return {};
        stbi_set_flip_vertically_on_load_thread(1);
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
            stbi_load_from_memory(source.GetData(), source.Num(), &width, &height, &channels, 4), stbi_image_free);
        stbi_set_flip_vertically_on_load_thread(0);
        if (!decoded)
            return {};
        image->Width = part.AlbedoWidth;
        image->Height = part.AlbedoHeight;
        image->Pixels.Append(decoded.get(), static_cast<i32>(part.AlbedoUploadBytes));
        return image;
    }

    u64 FFoliageStreamingPayload::GetCpuBytes() const noexcept
    {
        u64 bytes = sizeof(*this) + Vertices.GetAllocatedSize() + Indices.GetAllocatedSize() + Parts.GetAllocatedSize();
        for (const auto& part : Parts)
            bytes += part.AlbedoPath.GetAllocatedSize();
        return bytes;
    }

    u64 FFoliageStreamingPayload::GetGpuBytes() const noexcept
    {
        // Reserve allocation alignment in addition to the logical streams.
        u64 bytes = static_cast<u64>(Vertices.Num()) * sizeof(Vertex) + static_cast<u64>(Indices.Num()) * sizeof(u32) + 8192u;
        // Each draw part owns its texture, including duplicate paths. Count what
        // Upload actually allocates, rather than assuming an unimplemented cache.
        for (const auto& part : Parts)
            bytes += part.AlbedoGpuBytes;
        return bytes;
    }

    u64 FFoliageStreamingPayload::GetUploadBytes(f32 alphaCutoff) const noexcept
    {
        u64 bytes = static_cast<u64>(Vertices.Num()) * sizeof(Vertex) + static_cast<u64>(Indices.Num()) * sizeof(u32);
        for (const auto& part : Parts)
        {
            bytes += part.AlbedoUploadBytes;
            if (AlphaCoverageMips::SanitizeCutoff(alphaCutoff) > 0.0f && part.AlbedoWidth && part.AlbedoHeight)
            {
                // SetAlphaCoverageCutoff rebuilds level zero and its CPU mip
                // chain after the first upload. Reserve the full chain; the
                // measured upload uses the effective (possibly capped) levels.
                u32 width = part.AlbedoWidth, height = part.AlbedoHeight;
                while (true)
                {
                    bytes += static_cast<u64>(width) * height * 4u;
                    if (width == 1u && height == 1u)
                        break;
                    width = std::max(width / 2u, 1u);
                    height = std::max(height / 2u, 1u);
                }
            }
        }
        return bytes;
    }

    Ref<FFoliageStreamingPayload> LoadFoliageStreamingPayload(const std::filesystem::path& absolutePath)
    {
        FRepresentationIOStats ioStats;
        Assimp::Importer importer;
        importer.SetIOHandler(new FMeasuredAssimpIO(ioStats));
        importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 66.0f);
        importer.SetPropertyBool(AI_CONFIG_PP_PTV_KEEP_HIERARCHY, true);
        importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
        constexpr u32 flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                              aiProcess_JoinIdenticalVertices | aiProcess_ValidateDataStructure | aiProcess_FindInvalidData |
                              aiProcess_SortByPType | aiProcess_PreTransformVertices;
        const aiScene* scene = importer.ReadFile(absolutePath.string(), flags);
        if (!scene || !scene->HasMeshes())
            return {};

        auto payload = Ref<FFoliageStreamingPayload>::Create();
        payload->Bounds = { glm::vec3(std::numeric_limits<f32>::max()), glm::vec3(std::numeric_limits<f32>::lowest()) };
        std::string extension = absolutePath.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
        const bool flipUV = extension == ".obj"; // Model's authored OBJ convention (#1399).
        for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (!mesh || mesh->mNumVertices == 0 || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0)
                continue;
            if (static_cast<u64>(payload->Vertices.Num()) + mesh->mNumVertices > static_cast<u64>(std::numeric_limits<i32>::max()))
                return {};
            const u32 firstVertex = static_cast<u32>(payload->Vertices.Num());
            for (u32 i = 0; i < mesh->mNumVertices; ++i)
            {
                const aiVector3D& p = mesh->mVertices[i];
                const aiVector3D normal = mesh->HasNormals() ? mesh->mNormals[i] : aiVector3D(0.0f, 1.0f, 0.0f);
                const aiVector3D uv = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][i] : aiVector3D{};
                const glm::vec3 position(p.x, p.y, p.z);
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                    !std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z) ||
                    !std::isfinite(uv.x) || !std::isfinite(uv.y))
                    return {};
                payload->Vertices.Emplace(position, glm::vec3(normal.x, normal.y, normal.z),
                                          glm::vec2(uv.x, flipUV ? 1.0f - uv.y : uv.y));
                payload->Bounds.Min = glm::min(payload->Bounds.Min, position);
                payload->Bounds.Max = glm::max(payload->Bounds.Max, position);
            }
            FFoliageStreamingPart part;
            part.BaseIndex = static_cast<u32>(payload->Indices.Num());
            for (u32 i = 0; i < mesh->mNumFaces; ++i)
            {
                const aiFace& face = mesh->mFaces[i];
                if (face.mNumIndices != 3)
                    continue;
                if (payload->Indices.Num() > std::numeric_limits<i32>::max() - 3)
                    return {};
                for (u32 corner = 0; corner < 3; ++corner)
                {
                    if (face.mIndices[corner] >= mesh->mNumVertices)
                        return {};
                    payload->Indices.Add(firstVertex + face.mIndices[corner]);
                }
            }
            part.IndexCount = static_cast<u32>(payload->Indices.Num()) - part.BaseIndex;
            if (part.IndexCount == 0)
                continue;
            if (mesh->mMaterialIndex < scene->mNumMaterials)
            {
                const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
                aiString texture;
                if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texture) != AI_SUCCESS)
                    (void)material->GetTexture(aiTextureType_BASE_COLOR, 0, &texture);
                if (texture.length > 0)
                {
                    // Embedded textures need a separate cooked descriptor. Refuse
                    // this optional tier instead of losing the pinned plant card.
                    if (texture.C_Str()[0] == '*')
                        return {};
                    std::string textureName = texture.C_Str();
                    std::ranges::replace(textureName, '\\', '/');
                    const std::filesystem::path relativeTexture(textureName);
                    // Preserve Model's material-path containment rule when
                    // preparing through this CPU-only importer.
                    if (relativeTexture.is_absolute() ||
                        std::ranges::any_of(relativeTexture, [](const auto& component)
                                            { return component == ".."; }))
                        return {};
                    const auto texturePath = (absolutePath.parent_path() / relativeTexture).lexically_normal();
                    int width = 0, height = 0, channels = 0;
                    if (ProbeImage(texturePath, width, height, channels, ioStats) && width > 0 && height > 0)
                    {
                        std::error_code error;
                        const u64 sourceBytes = std::filesystem::file_size(texturePath, error);
                        const u64 imageBytes = static_cast<u64>(width) * static_cast<u64>(height) * 4u;
                        if (error || sourceBytes == 0 || sourceBytes > static_cast<u64>(std::numeric_limits<i32>::max()) ||
                            imageBytes > static_cast<u64>(std::numeric_limits<i32>::max()))
                            return {};
                        part.AlbedoPath = texturePath.generic_string();
                        // RGB may be padded by a backend; RGBA with a full mip
                        // chain plus alignment is a conservative admission size.
                        // Initial full-chain backing and replacement coverage
                        // backing coexist until retirement; include both plus
                        // backend alignment before any GPU creation.
                        part.AlbedoGpuBytes = static_cast<u64>(width) * static_cast<u64>(height) * 16u + 8192u;
                        part.AlbedoUploadBytes = imageBytes;
                        part.AlbedoSourceBytes = sourceBytes;
                        // Output RGBA + decoder/filter/16-bit/HDR conversion
                        // scratch, plus bounded immutable compressed bytes.
                        part.AlbedoDecodeBytes = imageBytes * 8u + sourceBytes * 2u + 65536u;
                        part.AlbedoWidth = static_cast<u32>(width);
                        part.AlbedoHeight = static_cast<u32>(height);
                    }
                }
            }
            payload->Parts.Add(std::move(part));
        }
        if (payload->Vertices.IsEmpty() || payload->Indices.IsEmpty() || payload->Parts.IsEmpty() ||
            static_cast<u64>(payload->Vertices.Num()) * sizeof(Vertex) > std::numeric_limits<u32>::max() ||
            static_cast<u64>(payload->Indices.Num()) * sizeof(u32) > std::numeric_limits<u32>::max())
            return {};
        payload->IOStats = ioStats;
        return payload;
    }
} // namespace OloEngine

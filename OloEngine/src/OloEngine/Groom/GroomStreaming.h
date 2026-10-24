#pragma once

#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include "OloEngine/Groom/GroomStrandRequest.h"

#include <algorithm>
#include <span>
#include <type_traits>
#include <vector>

namespace OloEngine
{
    struct FGroomStreamingStats
    {
        u32 Requested = 0;
        u32 DetailDraws = 0;
        u32 Pending = 0;
        u32 FallbackDraws = 0;
        /// Why this frame's last fallback draw was not the detail it asked for.
        EGroomStreamingFallback LastFallback = EGroomStreamingFallback::None;
        u64 Cancelled = 0;
        u64 Evictions = 0;
        u64 BaseCpuBytes = 0;
        u64 FloorGpuBytes = 0;
        u64 OptionalGpuBytes = 0;
    };

    // Immutable output of the existing CPU geometry builders. The temporary std
    // vectors are the builders' existing API boundary; retained storage uses TArray.
    class FPreparedGroomGeometry final : public FRepresentationPayload
    {
      public:
        TArray<GroomStrandVertex> Vertices;
        TArray<u32> Indices;
        TArray<u32> RootCurves;
        TArray<u32> StrandFirstIndex;
        TArray<GroomCasterStrand> CasterStrands;
        GroomStrandMeshStats Stats;
        Ref<GroomAsset> Groom;
        Ref<GroomBindingAsset> Binding;

        [[nodiscard]] u64 GetCpuBytes() const noexcept override
        {
            return static_cast<u64>(Vertices.GetAllocatedSize()) + Indices.GetAllocatedSize() +
                   RootCurves.GetAllocatedSize() + StrandFirstIndex.GetAllocatedSize() + CasterStrands.GetAllocatedSize();
        }

        template<typename T>
        [[nodiscard]] static std::span<const T> View(const TArray<T>& values)
        {
            return { values.GetData(), static_cast<sizet>(values.Num()) };
        }
    };

    // Vertices a preparation writes out at a time (the main-thread rest stream's chunk).
    inline constexpr u32 kGroomPrepareChunkVertices = 262144;

    // Only immutable asset/coating data is captured by the worker. No pose,
    // skeleton palette, solver, frame span, renderer object or GPU API is borrowed.
    [[nodiscard]] inline Ref<FPreparedGroomGeometry> PrepareGroomGeometry(const Ref<GroomAsset>& groom,
                                                                          const Ref<GroomBindingAsset>& binding,
                                                                          const GroomLodLevel* level,
                                                                          GroomStrandBuildSettings build,
                                                                          GroomCoatSettings settings)
    {
        auto result = Ref<FPreparedGroomGeometry>::Create();
        result->Groom = groom;
        result->Binding = binding;
        const GroomBuildSource source = level ? GroomBuildSource::FromLevel(*groom, *level)
                                              : GroomBuildSource::FromAsset(*groom);
        const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        std::vector<u32> roots;
        std::vector<u32> first;
        std::vector<GroomCasterStrand> casters;
        if (binding)
        {
            // The ribbon goes straight into the retained array, a chunk at a
            // time (the #1608 walk): built whole and then copied, a
            // two-million-strand coat's 2 GB of vertices was held twice, and
            // its preparation alone took the Samoyed's play scene over the
            // test process's 6 GB ceiling.
            class RetainSink final : public GroomStrandVertexSink
            {
              public:
                explicit RetainSink(TArray<GroomStrandVertex>& out)
                    : m_Out(out) {}
                bool Begin(u32 vertexCount) override
                {
                    m_Out.Reserve(static_cast<i32>(vertexCount));
                    return true;
                }
                bool Write(std::span<const GroomStrandVertex> chunk) override
                {
                    m_Out.Append(chunk.data(), static_cast<i32>(chunk.size()));
                    return true;
                }

              private:
                TArray<GroomStrandVertex>& m_Out;
            } sink(result->Vertices);
            GroomRestStreamMeasures measures;
            result->Stats = BuildGroomStrandRestMeshStreamed(source, build, *binding, sink, kGroomPrepareChunkVertices,
                                                             indices, roots, measures, &coat, &first, &casters);
            if (measures.SinkFailed)
            {
                return nullptr;
            }
        }
        else
        {
            result->Stats = BuildGroomStrandMesh(source, build, vertices, indices, nullptr, &coat, nullptr,
                                                 &first, &casters);
            result->Vertices.Append(vertices.data(), static_cast<i32>(vertices.size()));
            std::vector<GroomStrandVertex>().swap(vertices);
        }
        if (result->Vertices.IsEmpty() || indices.empty())
        {
            return nullptr;
        }
        // Each builder array is let go as soon as it is copied.
        const auto keep = [](auto& out, auto& built)
        {
            out.Append(built.data(), static_cast<i32>(built.size()));
            std::remove_reference_t<decltype(built)>().swap(built);
        };
        keep(result->Indices, indices);
        keep(result->RootCurves, roots);
        keep(result->StrandFirstIndex, first);
        keep(result->CasterStrands, casters);
        return result;
    }

    // Geometry costs are bounded by the source and both authored work limits.
    // Each segment emits four vertices and two six-index orders (beauty/shadow).
    // The CPU reservation covers legacy builder vectors plus retained output and
    // selection/clumping scratch; no disk I/O is claimed for a pinned base groom.
    [[nodiscard]] inline FRepresentationDescriptor DescribeGroomGeometry(const GroomStrandRequest& request)
    {
        const GroomBuildSource source = request.BuildSource();
        const u64 curves = source.Curves.GetCurveCount();
        const u64 points = source.Curves.GetPointCount();
        const u64 segments = std::min<u64>(request.Build.MaxSegments, points > curves ? points - curves : 0u);
        const u64 geometry = segments * (4u * sizeof(GroomStrandVertex) + 12u * sizeof(u32));
        FRepresentationDescriptor desc;
        desc.CpuBytes = FAssetByteSize::Estimate(geometry * 3u + curves * 1024u + 4096u);
        desc.DiskBytes = FAssetByteSize::Actual(0);
        desc.UploadBytes = geometry;
        desc.GpuBytes = geometry;
        return desc;
    }

    // Keep the same cooked source identities, maps and hash seed. Cards are
    // the shipped coarse representation; an uncooked groom keeps a bounded
    // strand subset, with the existing per-role width compensation.
    inline void ApplyGroomStreamingFloor(GroomStrandRequest& request, u32 fallbackStrands)
    {
        request.StreamingFloor = true;
        if (const GroomLodLevel* cards = request.Groom->FindLodLevel(GroomRepresentation::Card); cards != nullptr)
        {
            request.Lod.Representation = GroomRepresentation::Card;
            request.LodLevel = cards;
            // Cards already carry their clusters' coverage; retaining the
            // cooked tier avoids thinning away the fallback's silhouette.
            request.Build.MaxStrands = std::max(1u, std::min(request.Build.MaxStrands, cards->GetCurveCount()));
        }
        else
        {
            request.Lod.Representation = GroomRepresentation::Strand;
            request.LodLevel = nullptr;
            const u32 floor = std::clamp(fallbackStrands, 1u, 65536u);
            request.Build.MaxStrands = std::max(1u, std::min(request.Build.MaxStrands, floor));
            request.Build.MaxSegments = std::min(request.Build.MaxSegments, floor * 64u);
        }
        request.Build.MaxWidthCompensation = std::max(request.Build.MaxWidthCompensation, 8.0f);
    }
} // namespace OloEngine

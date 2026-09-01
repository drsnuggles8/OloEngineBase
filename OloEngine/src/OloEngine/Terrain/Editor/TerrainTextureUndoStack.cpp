#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"

#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        // Bytes per texel for the formats the terrain authoring path snapshots.
        // Deliberately a small explicit table rather than a generic size query: an
        // unlisted format must be REFUSED, because a wrong size here does not
        // corrupt anything visible — it just makes the byte budget wrong, which is
        // the kind of quiet drift that turns a bounded cache back into a leak.
        sizet BytesPerTexel(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                case ImageFormat::R8UI:
                    return 1;
                case ImageFormat::R16UI:
                case ImageFormat::RG8:
                    return 2;
                case ImageFormat::RGB8:
                    return 3;
                case ImageFormat::RGBA8:
                case ImageFormat::R32F:
                case ImageFormat::R32I:
                case ImageFormat::RG16UI:
                case ImageFormat::RG16F:
                    return 4;
                case ImageFormat::RG32F:
                case ImageFormat::RGBA16F:
                    return 8;
                case ImageFormat::RGBA32F:
                    return 16;
                default:
                    return 0;
            }
        }
    } // namespace

    TerrainTextureUndoStack::TerrainTextureUndoStack(u32 maxEntries, sizet maxBytes)
        : m_MaxEntries(std::max(maxEntries, 1u)), m_MaxBytes(std::max(maxBytes, static_cast<sizet>(1)))
    {
    }

    bool TerrainTextureUndoStack::Contains(SnapshotId id) const
    {
        if (id == kInvalidSnapshot)
        {
            return false;
        }
        return std::ranges::any_of(m_Entries, [id](const Entry& e)
                                   { return e.Id == id; });
    }

    TerrainTextureUndoStack::SnapshotId TerrainTextureUndoStack::Capture(const Ref<Texture2D>& source,
                                                                         u32 x, u32 y, u32 w, u32 h)
    {
        OLO_PROFILE_FUNCTION();

        if (!source || w == 0 || h == 0)
        {
            return kInvalidSnapshot;
        }

        const u32 srcWidth = source->GetWidth();
        const u32 srcHeight = source->GetHeight();
        if (x >= srcWidth || y >= srcHeight)
        {
            return kInvalidSnapshot;
        }

        // Clamp rather than reject: a brush rect at the map edge is legitimate, and
        // the clamped rect is what Restore() will blit back, so the two agree.
        w = std::min(w, srcWidth - x);
        h = std::min(h, srcHeight - y);

        const TextureSpecification& srcSpec = source->GetSpecification();
        const sizet texelBytes = BytesPerTexel(srcSpec.Format);
        if (texelBytes == 0)
        {
            OLO_CORE_ERROR("TerrainTextureUndoStack::Capture - Unsupported snapshot format {}",
                           static_cast<i32>(srcSpec.Format));
            return kInvalidSnapshot;
        }

        TextureSpecification spec;
        spec.Width = w;
        spec.Height = h;
        spec.Format = srcSpec.Format;
        spec.SRGB = srcSpec.SRGB;
        spec.GenerateMips = false;

        Ref<Texture2D> snapshot = Texture2D::Create(spec);
        if (!snapshot)
        {
            OLO_CORE_ERROR("TerrainTextureUndoStack::Capture - Failed to allocate a {}x{} snapshot", w, h);
            return kInvalidSnapshot;
        }

        RenderCommand::CopyImageSubDataRegion(
            source->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            static_cast<i32>(x), static_cast<i32>(y), 0,
            snapshot->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            0, 0, 0, w, h);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate);

        Entry entry;
        entry.Id = m_NextId++;
        entry.Snapshot = snapshot;
        entry.X = x;
        entry.Y = y;
        entry.Width = w;
        entry.Height = h;
        entry.Bytes = static_cast<sizet>(w) * h * texelBytes;

        m_BytesUsed += entry.Bytes;
        const SnapshotId id = entry.Id;
        m_Entries.push_back(std::move(entry));

        // After the push, so the newest snapshot is itself subject to the budget —
        // a single stroke larger than the whole budget must not be allowed to sit
        // outside it. Which means the entry just created may already be gone, and
        // the caller has to be told: an id that no longer resolves is indistinguishable
        // from a working one until the user presses undo and nothing happens.
        EvictUntilWithinBudget();
        if (!Contains(id))
        {
            return kInvalidSnapshot;
        }
        return id;
    }

    bool TerrainTextureUndoStack::Restore(SnapshotId id, Ref<Texture2D> dest) const
    {
        OLO_PROFILE_FUNCTION();

        if (id == kInvalidSnapshot || !dest)
        {
            return false;
        }

        const auto it = std::ranges::find_if(m_Entries, [id](const Entry& e)
                                             { return e.Id == id; });
        if (it == m_Entries.end() || !it->Snapshot)
        {
            return false;
        }

        // The destination may have been recreated since the snapshot was taken (a
        // terrain regenerated under the undo history), at a different resolution OR
        // in a different format. Either makes the copy wrong — a mismatched extent
        // corrupts an unrelated region, and glCopyImageSubData requires
        // format-compatible operands — so the step is refused rather than attempted.
        // EnsureFullCopy already treats format as part of the match; this is the
        // same rule on the restore side.
        if (it->X + it->Width > dest->GetWidth() || it->Y + it->Height > dest->GetHeight())
        {
            return false;
        }
        if (it->Snapshot->GetSpecification().Format != dest->GetSpecification().Format)
        {
            return false;
        }

        RenderCommand::CopyImageSubDataRegion(
            it->Snapshot->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            0, 0, 0,
            dest->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0,
            static_cast<i32>(it->X), static_cast<i32>(it->Y), 0,
            it->Width, it->Height);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::TextureFetch |
                                     MemoryBarrierFlags::ShaderImageAccess);

        // A restore writes LEVEL 0 and nothing else, so it has the same problem the
        // brush kernel does: the coarse levels still hold the state being undone.
        // Skipping this is not a subtle error — after undo the near field shows the
        // restored surface and the distance keeps the stroke, which is arguably
        // worse than not undoing at all. Caught by
        // TerrainGPUBrushVisualEvidenceTest, which passed before mip regeneration
        // existed only because every level was then uniformly stale.
        dest->RegenerateMips();
        return true;
    }

    bool TerrainTextureUndoStack::EnsureFullCopy(Ref<Texture2D>& slot, const Ref<Texture2D>& source)
    {
        OLO_PROFILE_FUNCTION();

        if (!source)
        {
            return false;
        }

        const u32 width = source->GetWidth();
        const u32 height = source->GetHeight();
        if (width == 0 || height == 0)
        {
            return false;
        }

        const TextureSpecification& srcSpec = source->GetSpecification();
        // Format is part of the match, not just the extent: a terrain rebuilt at
        // the same resolution in a different format would otherwise reuse a slot
        // the copy cannot legally target.
        if (!slot || slot->GetWidth() != width || slot->GetHeight() != height ||
            slot->GetSpecification().Format != srcSpec.Format)
        {
            TextureSpecification spec;
            spec.Width = width;
            spec.Height = height;
            spec.Format = srcSpec.Format;
            spec.SRGB = srcSpec.SRGB;
            spec.GenerateMips = false;

            slot = Texture2D::Create(spec);
            if (!slot)
            {
                OLO_CORE_ERROR("TerrainTextureUndoStack::EnsureFullCopy - Failed to allocate a {}x{} copy",
                               width, height);
                return false;
            }
        }

        RenderCommand::CopyImageSubDataRegion(
            source->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0, 0, 0, 0,
            slot->GetRHIHandle(), RendererAPI::TextureTargetType::Texture2D, 0, 0, 0, 0,
            width, height);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate);
        return true;
    }

    void TerrainTextureUndoStack::Release(SnapshotId id)
    {
        if (id == kInvalidSnapshot)
        {
            return;
        }

        const auto it = std::ranges::find_if(m_Entries, [id](const Entry& e)
                                             { return e.Id == id; });
        if (it == m_Entries.end())
        {
            return;
        }

        m_BytesUsed -= std::min(m_BytesUsed, it->Bytes);
        m_Entries.erase(it);
    }

    void TerrainTextureUndoStack::EvictUntilWithinBudget()
    {
        while (!m_Entries.empty() &&
               (m_Entries.size() > m_MaxEntries || m_BytesUsed > m_MaxBytes))
        {
            m_BytesUsed -= std::min(m_BytesUsed, m_Entries.front().Bytes);
            m_Entries.erase(m_Entries.begin());
            ++m_EvictionCount;
        }
    }
} // namespace OloEngine

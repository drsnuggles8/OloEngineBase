#pragma once

#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include <nlohmann/json.hpp>

namespace OloEngine::RepresentationStreaming
{
    // This attributes an optional geometry budget; it is not total device VRAM.
    // Physical backing/residency remain the renderer memory report's figures.
    [[nodiscard]] inline nlohmann::json ToJson(const FRepresentationStreamingStats& s)
    {
        return { { "optionalResidentGpuBytes", s.OptionalResidentGpuBytes },
                 { "pinnedResidentGpuBytes", s.PinnedResidentGpuBytes },
                 { "retiringGpuBytes", s.RetiringGpuBytes },
                 { "maxResidentGpuBytes", s.MaxResidentGpuBytes },
                 { "maxUploadBytesPerFrame", s.MaxUploadBytesPerFrame },
                 { "uploadedBytesThisFrame", s.UploadedBytesThisFrame },
                 { "pinnedUploadBytesThisFrame", s.PinnedUploadBytesThisFrame },
                 { "pendingCpuBytes", s.StagingCpuBytes },
                 { "maxStagingCpuBytes", s.MaxStagingCpuBytes },
                 { "unknownStagingCount", s.UnknownStagingCount },
                 { "pendingLoads", s.PendingLoads },
                 { "completedUnretrievedLoads", s.CompletedUnretrievedLoads },
                 { "abandonedRunningLoads", s.AbandonedRunningLoads },
                 { "heldCompletedLoads", s.HeldCompletedLoads },
                 { "deniedForResident", s.DeniedForResident },
                 { "deniedForUpload", s.DeniedForUpload },
                 { "deniedForStaging", s.DeniedForStaging },
                 { "preparationMicroseconds", s.PreparationMicroseconds },
                 { "readBytes", s.ReadBytes },
                 { "readMicroseconds", s.ReadMicroseconds },
                 { "unknownReadCount", s.UnknownReadCount },
                 { "completedLoads", s.CompletedLoads },
                 { "failedLoads", s.FailedLoads },
                 { "uploadMicroseconds", s.UploadMicroseconds },
                 { "actualUploadedBytes", s.ActualUploadedBytes } };
    }
} // namespace OloEngine::RepresentationStreaming

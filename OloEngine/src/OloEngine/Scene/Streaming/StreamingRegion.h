#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Asset/AssetByteSize.h"

#include <filesystem>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Why the last load request for a region did not start (issue #1365).
     *
     * A region stays in the state its last request left it in until a later
     * request is admitted or, for a proximity region, until it leaves its load
     * radius and nothing wants it any more.
     */
    enum class EStreamingAdmissionStatus : u8
    {
        None,     // the last request was admitted, or there has been none
        Deferred, // it would cross a byte budget now; it is retried on the next request
        Rejected  // it can never fit: its estimate alone exceeds MaxResidentMegabytes
    };

    enum class EStreamingAdmissionReason : u8
    {
        None,
        ResidentBudget,            // loaded + loading bytes plus this region would exceed MaxResidentMegabytes
        FrameBudget,               // bytes already admitted this frame plus this region would exceed MaxAdmittedMegabytesPerFrame
        LargerThanResidentBudget   // this region's estimate alone exceeds MaxResidentMegabytes
    };

    [[nodiscard]] const char* ToString(EStreamingAdmissionStatus status) noexcept;
    [[nodiscard]] const char* ToString(EStreamingAdmissionReason reason) noexcept;

    class StreamingRegion : public RefCounted
    {
      public:
        enum class State : u8
        {
            Unloaded, // No data in memory
            Loading,  // Background task in flight; its parsed file is the task's result
            Ready,    // Entities live in Scene
            Unloading // Entities being removed
        };

        StreamingRegion() = default;
        ~StreamingRegion() override = default;

        // Identity
        UUID m_RegionID;
        FString m_Name;
        // Native filesystem path passed to ParseRegionFile; retain Windows wide
        // characters without converting through an engine narrow string.
        std::filesystem::path m_SourcePath; // .oloregion file

        // Spatial bounds (axis-aligned)
        glm::vec3 m_BoundsMin{ 0.0f };
        glm::vec3 m_BoundsMax{ 0.0f };

        // State (guarded by SceneStreamer::m_RegionMutex)
        State m_State = State::Unloaded;

        // LRU tracking (frame number of last proximity hit)
        u64 m_LastUsedFrame = 0;

        // Entity tracking (filled after additive deserialize)
        TArray<UUID> m_EntityUUIDs;

        // Byte accounting (issue #1365). The .oloregion file size, read at discovery:
        // a prediction of what the region costs once resident, known before any load.
        // Unknown when the size could not be read — never zero.
        FAssetByteSize m_EstimatedSize;

        // Admission outcome of the last load request (guarded like m_State).
        EStreamingAdmissionStatus m_AdmissionStatus = EStreamingAdmissionStatus::None;
        EStreamingAdmissionReason m_AdmissionReason = EStreamingAdmissionReason::None;
        bool m_WarnedAdmission = false; // a rejection or unknown-size admission logs once per region

        // Request bookkeeping (guarded like m_State).
        bool m_LoadRequested = false;   // an explicit LoadRegion not yet admitted; Update retries it
        bool m_ManuallyLoaded = false;  // loaded by an explicit request: never freed for deferred demand
        bool m_UnloadRequested = false; // unload asked for while the region was being instantiated
        u64 m_InLoadRadiusFrame = 0;    // last frame an activation point was inside its load radius
    };

    [[nodiscard]] const char* ToString(StreamingRegion::State state) noexcept;
} // namespace OloEngine

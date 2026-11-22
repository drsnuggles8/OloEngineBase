#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReportJson.h"

namespace OloEngine
{
    namespace
    {
        using Json = nlohmann::json;

        [[nodiscard]] Json OptionalBytes(const std::optional<u64>& value)
        {
            return value ? Json(*value) : Json(nullptr);
        }

        [[nodiscard]] Json Totals(const MemoryTotals& totals)
        {
            return Json{ { "liveBytes", totals.LiveBytes },
                         { "retiringBytes", totals.RetiringBytes },
                         { "residentBytes", totals.ResidentBytes() },
                         { "committedBytes", totals.CommittedBytes },
                         { "estimatedBytes", totals.EstimatedBytes },
                         { "liveCount", totals.LiveCount },
                         { "retiringCount", totals.RetiringCount },
                         { "peakBytes", totals.PeakBytes },
                         { "windowPeakBytes", totals.WindowPeakBytes } };
        }

        [[nodiscard]] Json Backend(const RendererMemoryReport& report)
        {
            const MemoryReconciliation& reconciliation = report.Reconciliation;
            Json heaps = Json::array();
            for (const auto& heap : report.Observation.Heaps)
            {
                heaps.push_back(Json{ { "index", heap.Index },
                                      { "deviceLocal", heap.DeviceLocal },
                                      { "heapSizeBytes", heap.HeapSizeBytes },
                                      { "usageBytes", heap.UsageBytes },
                                      { "budgetBytes", heap.BudgetBytes },
                                      { "allocatorBlockBytes", heap.AllocatorBlockBytes },
                                      { "allocatorAllocationBytes", heap.AllocatorAllocationBytes } });
            }
            return Json{
                { "name", ToString(report.Observation.Backend) },
                { "reconciliation",
                  Json{ { "status", ToString(reconciliation.Status) },
                        { "observedAllocationBytes", OptionalBytes(reconciliation.ObservedAllocationBytes) },
                        { "trackedCommittedBytes", OptionalBytes(reconciliation.TrackedCommittedBytes) },
                        { "differenceBytes", reconciliation.DifferenceBytes ? Json(*reconciliation.DifferenceBytes) : Json(nullptr) } } },
                { "allocatorBlockBytes", report.Observation.HasAllocatorTotals ? Json(report.Observation.BlockBytes) : Json(nullptr) },
                { "residency", Json{ { "status", ToString(report.Observation.Residency) }, { "heaps", std::move(heaps) } } },
            };
        }
        [[nodiscard]] Json QualityConfig(const MemoryQualityConfig& config)
        {
            Json techniques = Json::array();
            for (u32 index = 0; index < kMemoryTechniqueCount; ++index)
            {
                const auto technique = static_cast<MemoryTechnique>(index);
                if (config.Has(technique))
                    techniques.push_back(ToString(technique));
            }
            return Json{ { "path", ToToken(config.Path) },
                         { "displayWidth", config.DisplayWidth },
                         { "displayHeight", config.DisplayHeight },
                         { "sceneWidth", config.SceneWidth },
                         { "sceneHeight", config.SceneHeight },
                         { "msaaSamples", config.MsaaSamples },
                         { "ssgiHalfResolution", config.SSGIHalfResolution },
                         { "transientPoolSpare", config.TransientPoolSpare },
                         { "optionalDetail", config.OptionalDetail },
                         { "techniques", std::move(techniques) } };
        }

        // Requested against realized quality (#1343). null when no renderer is admitting.
        [[nodiscard]] Json Admission(const std::optional<MemoryAdmissionSnapshot>& admission)
        {
            if (!admission)
                return nullptr;
            const MemoryAdmissionSnapshot& snapshot = *admission;

            Json rungs = Json::array();
            for (u32 rung = 0; rung < kMemoryLadderRungCount; ++rung)
            {
                if ((snapshot.ActiveRungMask & (1u << rung)) != 0u)
                    rungs.push_back(kMemoryLadder[rung].Name);
            }
            Json views = Json::array();
            for (const auto& view : snapshot.Views)
            {
                views.push_back(Json{ { "view", view.View },
                                      { "reduced", view.IsReduced() },
                                      { "requested", QualityConfig(view.Requested) },
                                      { "realized", QualityConfig(view.Realized) } });
            }
            return Json{
                { "reduced", snapshot.IsReduced() },
                { "reason", ToString(snapshot.Reason) },
                { "level", snapshot.Level },
                { "floorLevel", snapshot.FloorLevel },
                { "activeRungs", std::move(rungs) },
                { "budget",
                  Json{ { "source", ToString(snapshot.BudgetSource) },
                        { "bytes", OptionalBytes(snapshot.BudgetBytes) },
                        { "reserveBytes", snapshot.ReserveBytes },
                        { "residentBytes", snapshot.ResidentBytes },
                        { "retiringBytes", snapshot.RetiringBytes } } },
                { "predicted",
                  Json{ { "steadyBytes", snapshot.PredictedSteadyBytes },
                        { "peakBytes", snapshot.PredictedPeakBytes },
                        { "complete", snapshot.EstimateComplete } } },
                { "recovery",
                  Json{ { "progressFrames", snapshot.RecoveryProgressFrames }, { "requiredFrames", snapshot.RecoveryRequiredFrames } } },
                { "counters",
                  Json{ { "ticks", snapshot.Ticks },
                        { "transitions", snapshot.Transitions },
                        { "degradations", snapshot.Degradations },
                        { "recoveries", snapshot.Recoveries },
                        { "failedRecoveries", snapshot.FailedRecoveries },
                        { "readmissions", snapshot.Readmissions },
                        { "ticksAtLevel", snapshot.TicksAtLevel },
                        { "peakResidentBytes", snapshot.PeakResidentBytes } } },
                { "views", std::move(views) },
            };
        }
    } // namespace

    nlohmann::json RendererMemorySummaryToJson(const RendererMemoryReport& report)
    {
        return Json{ { "units", "bytes" },
                     { "gpu", Totals(report.Gpu) },
                     { "cpu", Totals(report.Cpu) },
                     { "aliases", Json{ { "count", report.AliasCount }, { "logicalBytes", report.AliasLogicalBytes }, { "orphanCount", report.OrphanAliasCount } } },
                     { "backend", Backend(report) },
                     { "admission", Admission(report.Admission) } };
    }

    nlohmann::json RendererMemoryReportToJson(const RendererMemoryReport& report)
    {
        Json json = RendererMemorySummaryToJson(report);

        Json owners = Json::array();
        for (const auto& row : report.Owners)
        {
            owners.push_back(Json{ { "owner", row.Owner.ToStdString() },
                                   { "lifetime", ToString(row.Lifetime) },
                                   { "gpuLiveBytes", row.GpuLiveBytes },
                                   { "gpuRetiringBytes", row.GpuRetiringBytes },
                                   { "cpuLiveBytes", row.CpuLiveBytes },
                                   { "cpuRetiringBytes", row.CpuRetiringBytes },
                                   { "committedBytes", row.CommittedBytes },
                                   { "estimatedBytes", row.EstimatedBytes },
                                   { "count", row.AllocationCount } });
        }
        json["owners"] = std::move(owners);

        Json capacity = Json::array();
        for (const auto& row : report.Capacity)
        {
            capacity.push_back(Json{ { "owner", row.Owner.ToStdString() },
                                     { "category", row.Category.ToStdString() },
                                     { "lifetime", ToString(row.Lifetime) },
                                     { "source", ToString(row.Source) },
                                     { "domain", row.IsGpu ? "gpu" : "cpu" },
                                     { "capacityBytes", OptionalBytes(row.CapacityBytes) },
                                     { "activeDemandBytes", OptionalBytes(row.ActiveDemandBytes) },
                                     { "aliasSavingsBytes", OptionalBytes(row.AliasSavingsBytes) },
                                     { "unknownReason", row.UnknownReason.IsEmpty() ? Json(nullptr) : Json(row.UnknownReason.ToStdString()) } });
        }
        json["capacity"] = std::move(capacity);
        return json;
    }
} // namespace OloEngine

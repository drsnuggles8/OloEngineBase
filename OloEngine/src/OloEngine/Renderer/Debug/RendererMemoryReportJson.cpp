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
    } // namespace

    nlohmann::json RendererMemorySummaryToJson(const RendererMemoryReport& report)
    {
        return Json{ { "units", "bytes" },
                     { "gpu", Totals(report.Gpu) },
                     { "cpu", Totals(report.Cpu) },
                     { "aliases", Json{ { "count", report.AliasCount }, { "logicalBytes", report.AliasLogicalBytes }, { "orphanCount", report.OrphanAliasCount } } },
                     { "backend", Backend(report) } };
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

#pragma once

// Pure shaping + compare math for the olo_render_validate MCP tool (issue
// #607): an on-demand render-graph frame validation sweep, plus an optional
// bit-exact texture compare (the "assert HZB mip0 == scene depth bitwise
// after GTAOPass" ask from the GTAO hunt).
//
// The default sweep gathers, from the live graph: the compiled resource-hazard
// validation, the barrier/build diagnostics and execute-path resolve failures
// the graph already records, and a physical-identity report — every resource's
// resolved physical backing grouped by base name, with resources that are
// CONSUMED but resolve to no physical backing flagged. That last verdict rests
// on the IDENTITY currency, never on the backend-native handle (issue #890);
// see McpNativeHandle.h for why the two are not interchangeable. The optional {compare:{a,b,...}}
// mode reads both textures back as floats and compares bit-exactly (with the
// first differing texels listed), optionally AS OF a given pass via the
// afterPass snapshot.
//
// The handler in McpToolsRender.cpp gathers everything on the main thread and
// pre-stringifies engine enums; everything below is free functions over PODs
// with NO GL / renderer / editor dependency, unit-tested headlessly — the
// same split McpRenderProbePixel.h / McpRenderGraphTopology.h use.

#include "MCP/McpNativeHandle.h"

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP::RenderValidate
{
    using Json = nlohmann::json;

    // ---- Default-sweep input (pre-stringified by the handler) --------------

    struct HazardInfo
    {
        std::string Kind; // "ReadAfterWrite", "Cycle", ... (pre-resolved)
        std::string Resource;
        std::string Producer;
        std::string Consumer;
        std::string Message;
    };

    struct DiagnosticInfo
    {
        std::string Kind;
        std::string Pass;
        std::string Resource;
        std::string Message;
    };

    struct ResolveFailureInfo
    {
        std::string Pass;
        std::string Reason;
        u32 Count = 0;
    };

    // One registered resource's physical identity this frame.
    //
    // TWO currencies, and the distinction is the whole point (issue #890, ADR
    // 0011 amendments (77) and (90)). `Native*` is what a RenderDoc / RGP
    // capture shows and is DISPLAY ONLY; `*Identity` and `TextureHasStorage`
    // are what a verdict may rest on. The predicate below used to read a
    // GL-shaped `u32` native id. Measured live, that was wrong on BOTH
    // backends in opposite directions: on Vulkan it flagged 11 backed,
    // readable, scene-content-carrying resources (a native 0 is what every
    // framebuffer attachment reports there, by design) and returned a false
    // `ok: false`; on OpenGL it returned `ok: true` while MISSING two
    // genuinely storage-less resources whose recycled GL names were non-zero.
    struct ResourceIdentity
    {
        std::string Name;
        // Display currency: the FULL backend-native handle (GL name / VkImage).
        // 0 is legitimate here — it can confirm backing, never deny it.
        u64 NativeTextureHandle = 0;
        u64 NativeBufferHandle = 0;
        // Decision currency: RHI::HashKey of the resolved identity, 0 = none.
        u64 TextureIdentity = 0;
        u64 BufferIdentity = 0;
        // The backend's own answer to "is there storage here" (a successful
        // dimension query through the facade). Conclusive on both backends.
        bool TextureHasStorage = false;
        bool HasProducers = false;
        bool HasConsumers = false;
        std::string LastWriter; // empty when none recorded
    };

    // Strip a versioned name ("SceneColor@ParticlePass") to its base name.
    [[nodiscard]] inline std::string_view BaseName(std::string_view name) noexcept
    {
        const sizet at = name.find('@');
        return at == std::string_view::npos ? name : name.substr(0, at);
    }

    [[nodiscard]] inline bool HasTextureBacking(const ResourceIdentity& r) noexcept
    {
        return HasBacking(r.TextureIdentity, r.TextureHasStorage, r.NativeTextureHandle);
    }

    [[nodiscard]] inline bool HasBufferBacking(const ResourceIdentity& r) noexcept
    {
        return MCP::HasBufferBacking(r.BufferIdentity, r.NativeBufferHandle);
    }

    // A resource that is read by at least one pass but resolves to no
    // physical backing — a reader sampling nothing (or a stale binding).
    [[nodiscard]] inline bool IsUnbackedConsumed(const ResourceIdentity& r) noexcept
    {
        return r.HasConsumers && !HasTextureBacking(r) && !HasBufferBacking(r);
    }

    // The key that answers "did these two versions resolve to the same physical
    // object". The IDENTITY when there is one — it is the only currency that
    // distinguishes a recycled name from the same object on either backend —
    // and the native handle only as the fallback for a resource imported as a
    // bare native id, which has no identity to give. Returns 0 for "unbacked",
    // which callers exclude from the distinctness count rather than treating
    // as a physical object shared by everything unbacked.
    [[nodiscard("The physical key is the version-group comparison; discarding it compares nothing.")]]
    inline u64 PhysicalKey(const ResourceIdentity& r) noexcept
    {
        // Gated on BACKING, not merely on "an id is present". A transient the
        // planner never allocated still carries a live identity, and counting
        // that as a distinct physical resource would let VersionGroupsJson
        // report `multiplePhysicalIds` for a group whose versions are all
        // unbacked — contradicting the `backed: false` printed beside them.
        if (HasTextureBacking(r))
            return r.TextureIdentity != 0 ? r.TextureIdentity : r.NativeTextureHandle;
        if (HasBufferBacking(r))
            return r.BufferIdentity != 0 ? r.BufferIdentity : r.NativeBufferHandle;

        return 0;
    }

    // Group identities by base name and report every group where versions
    // resolve to MORE THAN ONE distinct physical resource — legitimate for
    // copy-on-write versioning, but exactly the map an agent needs to answer
    // "which physical texture did this reader actually see". Pure and
    // deterministic (insertion order preserved).
    [[nodiscard]] inline Json VersionGroupsJson(const std::vector<ResourceIdentity>& identities)
    {
        Json groups = Json::array();
        std::vector<std::string> seenBases;
        for (const auto& identity : identities)
        {
            const std::string base(BaseName(identity.Name));
            if (std::find(seenBases.begin(), seenBases.end(), base) != seenBases.end())
                continue;
            seenBases.push_back(base);

            Json versions = Json::array();
            std::vector<u64> distinctKeys;
            for (const auto& other : identities)
            {
                if (BaseName(other.Name) != base)
                    continue;
                Json v;
                v["name"] = other.Name;
                if (other.NativeTextureHandle != 0)
                    v["nativeTextureHandle"] = NativeHandleHex(other.NativeTextureHandle);
                if (other.NativeBufferHandle != 0)
                    v["nativeBufferHandle"] = NativeHandleHex(other.NativeBufferHandle);
                if (const std::string token = IdentityToken(other.TextureIdentity); !token.empty())
                    v["textureIdentity"] = token;
                if (const std::string token = IdentityToken(other.BufferIdentity); !token.empty())
                    v["bufferIdentity"] = token;
                v["backed"] = HasTextureBacking(other) || HasBufferBacking(other);
                if (!other.LastWriter.empty())
                    v["lastWriter"] = other.LastWriter;
                versions.push_back(std::move(v));
                if (const u64 key = PhysicalKey(other);
                    key != 0 && std::find(distinctKeys.begin(), distinctKeys.end(), key) == distinctKeys.end())
                {
                    distinctKeys.push_back(key);
                }
            }
            if (versions.size() <= 1)
                continue; // a single version can't alias-split
            Json group;
            group["baseName"] = base;
            group["versions"] = std::move(versions);
            group["multiplePhysicalIds"] = distinctKeys.size() > 1;
            groups.push_back(std::move(group));
        }
        return groups;
    }

    // ---- Bit-exact compare --------------------------------------------------

    struct CompareRequest
    {
        std::string A;
        std::string B;
        u32 MipA = 0;
        u32 MipB = 0;
        // Explicit layer selectors. When Has* is false the handler applies
        // the target's OWN view layer (a CSM cascade view compares ITS
        // cascade, never silently layer 0 — the capture/probe/stats rule).
        u32 LayerA = 0;
        u32 LayerB = 0;
        bool HasLayerA = false;
        bool HasLayerB = false;
        std::string AfterPass; // optional: snapshot both AS OF this pass
    };

    struct DiffTexel
    {
        u32 X = 0;
        u32 Y = 0;
        f32 ValueA = 0.0f;
        f32 ValueB = 0.0f;
    };

    struct CompareResult
    {
        std::string Error; // non-empty = the compare could not run
        u32 Width = 0;     // compared region (min of both mips)
        u32 Height = 0;
        u32 WidthA = 0;
        u32 HeightA = 0;
        u32 WidthB = 0;
        u32 HeightB = 0;
        std::string FormatA;
        std::string FormatB;
        u64 ComparedTexels = 0;
        u64 DifferingTexels = 0;
        bool BitwiseEqual = false;
        f64 MaxAbsDiff = 0.0;
        std::vector<DiffTexel> FirstDiffs; // first few differing texels, row-major
    };

    inline constexpr u32 kMaxReportedDiffs = 8;

    // Compare channel 0 of two float readbacks bit-exactly over the
    // overlapping top-left region. `a`/`b` are row-major, TOP-LEFT origin
    // (the handler flips GL rows before calling), one value per texel.
    [[nodiscard]] inline CompareResult CompareFloatBuffers(const std::vector<f32>& a, u32 widthA, u32 heightA,
                                                           const std::vector<f32>& b, u32 widthB, u32 heightB)
    {
        CompareResult result;
        result.WidthA = widthA;
        result.HeightA = heightA;
        result.WidthB = widthB;
        result.HeightB = heightB;
        result.Width = std::min(widthA, widthB);
        result.Height = std::min(heightA, heightB);
        if (result.Width == 0 || result.Height == 0 ||
            a.size() < static_cast<sizet>(widthA) * heightA ||
            b.size() < static_cast<sizet>(widthB) * heightB)
        {
            result.Error = "Degenerate compare region (a target has no storage at the requested mip).";
            return result;
        }

        for (u32 y = 0; y < result.Height; ++y)
        {
            for (u32 x = 0; x < result.Width; ++x)
            {
                const f32 va = a[static_cast<sizet>(y) * widthA + x];
                const f32 vb = b[static_cast<sizet>(y) * widthB + x];
                if (std::bit_cast<u32>(va) == std::bit_cast<u32>(vb))
                    continue;
                ++result.DifferingTexels;
                if (std::isfinite(va) && std::isfinite(vb))
                    result.MaxAbsDiff = std::max(result.MaxAbsDiff,
                                                 std::abs(static_cast<f64>(va) - static_cast<f64>(vb)));
                if (result.FirstDiffs.size() < kMaxReportedDiffs)
                    result.FirstDiffs.push_back(DiffTexel{ x, y, va, vb });
            }
        }
        result.ComparedTexels = static_cast<u64>(result.Width) * result.Height;
        result.BitwiseEqual = result.DifferingTexels == 0;
        return result;
    }

    [[nodiscard]] inline Json CompareResultJson(const CompareRequest& request, const CompareResult& result)
    {
        Json j;
        j["a"] = Json{ { "name", request.A }, { "mip", request.MipA }, { "width", result.WidthA }, { "height", result.HeightA }, { "format", result.FormatA } };
        j["b"] = Json{ { "name", request.B }, { "mip", request.MipB }, { "width", result.WidthB }, { "height", result.HeightB }, { "format", result.FormatB } };
        if (request.LayerA != 0)
            j["a"]["layer"] = request.LayerA;
        if (request.LayerB != 0)
            j["b"]["layer"] = request.LayerB;
        if (!request.AfterPass.empty())
            j["afterPass"] = request.AfterPass;
        if (!result.Error.empty())
        {
            j["error"] = result.Error;
            return j;
        }
        j["comparedRegion"] = Json{ { "width", result.Width }, { "height", result.Height } };
        j["comparedTexels"] = result.ComparedTexels;
        j["bitwiseEqual"] = result.BitwiseEqual;
        j["differingTexels"] = result.DifferingTexels;
        if (result.DifferingTexels > 0)
        {
            j["maxAbsDiff"] = result.MaxAbsDiff;
            Json diffs = Json::array();
            for (const auto& d : result.FirstDiffs)
            {
                Json e;
                e["x"] = d.X;
                e["y"] = d.Y;
                e["a"] = std::isfinite(d.ValueA) ? Json(d.ValueA) : Json(std::isnan(d.ValueA) ? "NaN" : "Inf");
                e["b"] = std::isfinite(d.ValueB) ? Json(d.ValueB) : Json(std::isnan(d.ValueB) ? "NaN" : "Inf");
                e["aBits"] = std::bit_cast<u32>(d.ValueA);
                e["bBits"] = std::bit_cast<u32>(d.ValueB);
                diffs.push_back(std::move(e));
            }
            j["firstDiffs"] = std::move(diffs);
        }
        j["note"] = "bitwiseEqual compares channel 0 bit patterns over the overlapping top-left region. "
                    "A D24 source quantizes on float readback, so cross-format compares against D24 are "
                    "only bit-exact when both sides went through the same conversion; D32F/R32F pairs "
                    "compare exactly.";
        return j;
    }

    // ---- Whole-reply assembly ----------------------------------------------

    [[nodiscard]] inline Json BuildValidateJson(const std::vector<HazardInfo>& hazards,
                                                const std::vector<DiagnosticInfo>& barrierDiagnostics,
                                                const std::vector<DiagnosticInfo>& buildDiagnostics,
                                                const std::vector<ResolveFailureInfo>& resolveFailures,
                                                const std::vector<ResourceIdentity>& identities)
    {
        Json j;

        Json hazardArray = Json::array();
        for (const auto& h : hazards)
        {
            Json e;
            e["kind"] = h.Kind;
            e["resource"] = h.Resource;
            if (!h.Producer.empty())
                e["producer"] = h.Producer;
            if (!h.Consumer.empty())
                e["consumer"] = h.Consumer;
            e["message"] = h.Message;
            hazardArray.push_back(std::move(e));
        }
        j["hazardCount"] = static_cast<u32>(hazards.size());
        j["hazards"] = std::move(hazardArray);

        const auto diagnosticsArray = [](const std::vector<DiagnosticInfo>& diagnostics)
        {
            Json arr = Json::array();
            for (const auto& d : diagnostics)
            {
                Json e;
                e["kind"] = d.Kind;
                if (!d.Pass.empty())
                    e["pass"] = d.Pass;
                if (!d.Resource.empty())
                    e["resource"] = d.Resource;
                e["message"] = d.Message;
                arr.push_back(std::move(e));
            }
            return arr;
        };
        j["barrierDiagnostics"] = diagnosticsArray(barrierDiagnostics);
        j["buildDiagnostics"] = diagnosticsArray(buildDiagnostics);

        Json failures = Json::array();
        for (const auto& f : resolveFailures)
        {
            Json e;
            e["pass"] = f.Pass;
            e["reason"] = f.Reason;
            e["count"] = f.Count;
            failures.push_back(std::move(e));
        }
        j["resolveFailures"] = std::move(failures);

        // Physical identity: consumed-but-unbacked resources are the "readers
        // did not resolve to what the writer produced" red flag; version
        // groups map the copy-on-write aliasing for the rest.
        Json unbacked = Json::array();
        for (const auto& identity : identities)
        {
            if (IsUnbackedConsumed(identity))
                unbacked.push_back(identity.Name);
        }
        j["consumedButUnbacked"] = std::move(unbacked);
        j["versionGroups"] = VersionGroupsJson(identities);

        const bool clean = hazards.empty() && resolveFailures.empty() && j["consumedButUnbacked"].empty();
        j["ok"] = clean;
        return j;
    }
} // namespace OloEngine::MCP::RenderValidate

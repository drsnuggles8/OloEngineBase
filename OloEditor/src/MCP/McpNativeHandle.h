#pragma once

// =============================================================================
// McpNativeHandle.h — how an MCP reply spells the TWO resource currencies.
//
// Issue #890. ADR 0011 amendment (77) settled that a diagnostic surfaces BOTH
// currencies; (89) settled which one it may DECIDE on. This header is the
// spelling half of that, shared by every tool that prints either:
//
//   * the backend-NATIVE handle — a GL name or a `VkImage`, the value a
//     RenderDoc / RGP capture shows. It is `u64` and rendered as hex because a
//     `VkImage` is a 64-bit pointer-shaped value: truncating one into a `u32`
//     turns "no answer" into an answer that passes a validity check, which is
//     the exact failure this file exists to stop. `0` is LEGITIMATE here — a
//     Vulkan framebuffer has no object under dynamic rendering (amendment
//     (83)), and every Vulkan texture class returns 0 from `GetRendererID()`
//     by design — so it can confirm backing and must never deny it.
//
//   * the IDENTITY — `RHI::ResourceHandle`, packed by `RHI::HashKey` into
//     `(Generation << 32) | Index`. This is the currency that answers "is
//     there an object here" and "are these two the same object" on every
//     backend, and therefore the only one a verdict may rest on.
//
// Kept free of every renderer / editor dependency (Base.h and the standard
// library only) so the pure shaping headers that include it — McpRenderValidate.h,
// McpRenderGraphTopology.h — stay headlessly unit-testable, which is the split
// the whole MCP layer follows. The handler side converts an
// `RHI::ResourceHandle` into these u64s through `OloEngine::Debug`; nothing
// here knows the RHI types exist.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <format>
#include <string>

namespace OloEngine::MCP
{
    // A native object handle as the hex a capture tool shows. Always the full
    // 64 bits — see the header comment on why this is not a u32.
    [[nodiscard]] inline std::string NativeHandleHex(u64 nativeHandle)
    {
        return std::format("0x{:X}", nativeHandle);
    }

    // An identity key (RHI::HashKey packing) as the "#index:generation" token
    // the engine's own fmt formatter for RHI::Handle prints, so a value copied
    // out of an MCP reply matches what a log line spells. Returns an empty
    // string for 0 (no identity), which every caller treats as "omit the key"
    // rather than printing a token that names nothing.
    [[nodiscard]] inline std::string IdentityToken(u64 identityKey)
    {
        if (identityKey == 0)
            return {};
        const u32 index = static_cast<u32>(identityKey & 0xFFFFFFFFull);
        const u32 generation = static_cast<u32>(identityKey >> 32u);
        return std::format("#{}:{}", index, generation);
    }

    // "Is there a GPU object behind this resource?" — the predicate every
    // verdict in the MCP layer must use, and the one #890 got wrong.
    //
    // The ORDER of these three questions is the whole fix, and getting it
    // subtly wrong produces the opposite bug. An identity means the backend
    // COULD be asked, so its answer — `hasStorage`, from a dimension query
    // through the facade — is FINAL, in both directions. An identity may not
    // "confirm" backing over a negative storage answer: a render-graph
    // resource can carry a perfectly valid handle and still have no storage
    // this frame (a transient the planner never allocated), and calling that
    // backed is exactly the false negative that made this tool answer
    // `ok: true` on OpenGL while two consumed resources had nothing behind
    // them.
    //
    // The native handle is reached only when there is no identity to ask
    // about — a resource imported as a bare native id. There, a non-zero name
    // is all the evidence available. It can never DENY backing, because 0 is
    // a legitimate native handle on Vulkan.
    [[nodiscard]] inline constexpr bool HasBacking(u64 identityKey, bool hasStorage, u64 nativeHandle) noexcept
    {
        if (identityKey != 0)
            return hasStorage;

        return nativeHandle != 0;
    }

    // Buffers have no dimension query to ask, so the identity is the whole
    // answer for them and a native name is the fallback. Kept as its own
    // spelling rather than passing `hasStorage = true` above, which would read
    // like a claim nobody verified.
    [[nodiscard]] inline constexpr bool HasBufferBacking(u64 identityKey, u64 nativeHandle) noexcept
    {
        return identityKey != 0 || nativeHandle != 0;
    }
} // namespace OloEngine::MCP

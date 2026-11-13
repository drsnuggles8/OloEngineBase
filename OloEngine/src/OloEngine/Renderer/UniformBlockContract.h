#pragma once

#include "OloEngine/Core/Base.h"

#include <string_view>

// The uniform block binding contract (ADR 0025 §5a, issue #1330).
//
// A shader reads whichever buffer occupies a binding point when the draw or
// dispatch is recorded, and several binding points host more than one block
// layout. "A buffer is bound there" is therefore not enough: the vegetation
// deformer once read the skeletal deformer's 48-byte block at binding 65 as its
// own 96-byte one and chased the bytes past its end as device addresses
// (#1437). This is the check that would have refused that dispatch: before a
// program reads a uniform block, the occupant's byte range and declared layout
// are compared with what the program's SPIR-V reflection says it declares.
//
// The comparison is pure and lives here; each backend calls it where it already
// looks the occupant up (the Vulkan root-data writer, the OpenGL draw entry
// points), so this adds no second record of what is bound.
namespace OloEngine::BindingContract
{
    // Identity of a GLSL block name: FNV-1a, never 0 for a non-empty name, so 0
    // can mean "no layout declared".
    [[nodiscard]] constexpr u32 HashBlockName(std::string_view name) noexcept
    {
        if (name.empty())
            return 0u;
        u32 hash = 2166136261u;
        for (const char c : name)
        {
            hash ^= static_cast<u8>(c);
            hash *= 16777619u;
        }
        return hash == 0u ? 1u : hash;
    }

    // What a program declares at one uniform binding point.
    struct BlockRequirement
    {
        u32 Binding = 0;
        u32 Size = 0;     // the block's declared size in bytes
        u32 NameHash = 0; // HashBlockName of the GLSL block name
    };

    // What a buffer says about itself. A producer on a binding point that hosts
    // more than one layout declares the GLSL block it was built for; the alias
    // is for the few layouts two shaders spell with two block names (`Camera`
    // and `CameraMatrices` are one layout).
    struct BlockDeclaration
    {
        u32 NameHash = 0;
        u32 AliasHash = 0;

        [[nodiscard]] constexpr bool IsDeclared() const noexcept
        {
            return NameHash != 0u;
        }
        [[nodiscard]] constexpr bool Names(u32 nameHash) const noexcept
        {
            return nameHash != 0u && (nameHash == NameHash || nameHash == AliasHash);
        }
    };

    enum class Verdict : u8
    {
        Satisfied,
        // The occupant is smaller than the block: the shader would read past it.
        Undersized,
        // The occupant declares another block: every field would be misread.
        WrongLayout,
    };

    // An occupant that declares no block is judged on its byte range alone.
    // Which buffers must declare is a rule about the engine's sources, not
    // about a draw: every UniformBuffer::Create in the engine and the editor on
    // a binding point that hosts several layouts
    // (ShaderBindingLayout::IsMultiLayoutUBOBinding) passes its block name, and
    // UniformBlockContractSourceTest fails when one does not. A test's ad-hoc
    // buffer may stay undeclared.
    [[nodiscard]] constexpr Verdict Check(const BlockRequirement& required, u32 occupantSize,
                                          const BlockDeclaration& occupant) noexcept
    {
        if (occupantSize < required.Size)
            return Verdict::Undersized;
        if (!occupant.IsDeclared())
            return Verdict::Satisfied;
        return occupant.Names(required.NameHash) ? Verdict::Satisfied : Verdict::WrongLayout;
    }

    [[nodiscard]] constexpr const char* ToString(Verdict verdict) noexcept
    {
        switch (verdict)
        {
            case Verdict::Satisfied:
                return "satisfied";
            case Verdict::Undersized:
                return "occupant smaller than the block";
            case Verdict::WrongLayout:
                return "occupant holds another block layout";
        }
        return "unknown";
    }

    // Whether the backends run the check. On unless OLO_BINDING_CONTRACT=0,
    // which exists to measure what the check costs and as the off arm of its
    // own negative control.
    [[nodiscard]] bool Enabled();

    // One refused block read. Logged once per (program, binding, verdict,
    // occupant) at error level and always counted; safe from recording workers.
    void ReportViolation(std::string_view program, const BlockRequirement& required, std::string_view requiredBlock,
                         u32 occupantSize, std::string_view occupantBlock, Verdict verdict);

    // A backend that keys its record of a program's blocks on a native program
    // name hears here when that name is retired (Shader::UnregisterProgram),
    // because the driver reissues names.
    using ProgramRetiredHandler = void (*)(u32 programID);
    void SetProgramRetiredHandler(ProgramRetiredHandler handler);
    void NotifyProgramRetired(u32 programID);

    // Refused block reads since the last reset, for tests and diagnostics.
    [[nodiscard]] u64 ViolationCount();
    // ...and how many of them were raised while recording a parallel item: the
    // check runs in every recording context, a worker's included.
    [[nodiscard]] u64 WorkerViolationCount();
    void ResetViolationCount();
} // namespace OloEngine::BindingContract

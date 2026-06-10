// =============================================================================
// FuzzSpirvCross.cpp
//
// libFuzzer harness for SPIRV-Cross's GLSL decompilation path. The engine
// compiles GLSL -> SPIR-V via shaderc at build time and then runs the
// SPIR-V back through SPIRV-Cross's `CompilerGLSL` for reflection + GL-side
// decompile (see `OpenGLShader::CompileOrGetOpenGLBinaries`).
//
// Production contract: shaderc only ever hands SPIRV-Cross *valid* SPIR-V.
// SPIRV-Cross's maintainer has explicitly confirmed this is the library's
// intended contract — see KhronosGroup/SPIRV-Cross#2635 (closed WONTFIX
// 2026-05-21): "The parser is not intended to exhaustively deal with any
// invalid SPIR-V input … SPIRV-Cross is not a bad clone of spirv-val."
// Feeding raw garbage would just rediscover the unbounded `ir.ids[id]`
// indexing and unchecked opcode-operand reads documented there. OloEngine's
// own triage of this exact harness is in issue #240 (closed) — we can't
// patch the Vulkan SDK from this repo, so we mirror production: every input
// is run through `spvValidateBinary` (SPIRV-Tools, already linked via the
// Vulkan SDK) before being handed to `CompilerGLSL`. Invalid SPIR-V is
// dropped on the floor.
//
// What this still catches: crashes / UAFs / OOBs inside SPIRV-Cross when
// reflecting or decompiling *valid* SPIR-V, and any bug in OloEngine's own
// SPIRV-Cross usage that would fire on a real shader. Bugs of either kind
// we can actually fix (the latter directly, the former by reporting upstream
// with a real-world reproducer instead of a malformed-bytes one).
//
// Exceptions from SPIRV-Cross are expected (some edge-case-but-valid SPIR-V
// still throws `CompilerError`); the harness swallows them — a crash / UAF /
// OOB is the only thing that should terminate the process.
// =============================================================================

#include "OloEnginePCH.h"
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv-tools/libspirv.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

    // Single shared validator context — `spvContextCreate` is non-trivial
    // (allocates an opcode table) and the libFuzzer runner reuses the process
    // across millions of `LLVMFuzzerTestOneInput` calls.
    struct SpvValidatorContext
    {
        spv_context m_Context;

        SpvValidatorContext()
            // SPV_ENV_VULKAN_1_0 matches shaderc's default target in
            // `OpenGLShader::CompileOrGetVulkanBinaries`. If the engine ever
            // bumps shaderc's `SetTargetEnvironment` call, bump this too — they
            // need to stay in lockstep so the harness accepts exactly what
            // production produces.
            : m_Context(spvContextCreate(SPV_ENV_VULKAN_1_0))
        {
        }
        ~SpvValidatorContext()
        {
            spvContextDestroy(m_Context);
        }
    };

    SpvValidatorContext& Validator()
    {
        static SpvValidatorContext s_Validator;
        return s_Validator;
    }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // SPIR-V is a stream of 32-bit words with a 5-word (20-byte) header.
    // Cap at 1 MiB; the largest shader in the engine is well below 64 KiB
    // of SPIR-V, so 1 MiB is generous without triggering OOM.
    if (size < 20 || size > static_cast<size_t>(1 << 20))
        return 0;

    // SPIR-V must be word-aligned. We could pad like the old harness did,
    // but valid SPIR-V is always a whole-word multiple — a non-multiple
    // length is itself a malformed input. Skipping is consistent with what
    // `spvValidateBinary` would do, just cheaper.
    if ((size & 0x3u) != 0)
        return 0;

    std::vector<uint32_t> words(size / 4);
    std::memcpy(words.data(), data, size);

    // Pre-validate. `spvValidateBinary` is a pure data-in / status-out call
    // and catches the malformed-header / bad-opcode / truncated-instruction
    // cases that historically crashed `CompilerGLSL`. If it rejects the
    // input, drop it — production code never sees invalid SPIR-V either.
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t validation = spvValidateBinary(
        Validator().m_Context,
        words.data(),
        words.size(),
        &diagnostic);
    if (diagnostic != nullptr)
        spvDiagnosticDestroy(diagnostic);
    if (validation != SPV_SUCCESS)
        return 0;

    try
    {
        spirv_cross::CompilerGLSL compiler(std::move(words));

        // Flip a couple of reflection-heavy surfaces before decompile so
        // we exercise the resource / type graph even when the compile path
        // bails early.
        (void)compiler.get_shader_resources();
        (void)compiler.get_entry_points_and_stages();

        (void)compiler.compile();
    }
    catch (const std::exception&)
    {
        // Edge-case-but-valid SPIR-V can still raise `spirv_cross::
        // CompilerError` (derives from std::exception) — e.g. unsupported
        // decorations / extensions. That's the library's normal
        // error-reporting path — not a bug in the engine.
    }
    catch (...)
    {
        // Safety net: some exception types in SPIRV-Cross don't derive
        // from std::exception. Still not a crash.
    }

    return 0;
}

// =============================================================================
// FuzzSpirvCross.cpp
//
// libFuzzer harness for SPIRV-Cross's GLSL decompilation path. The engine
// compiles GLSL -> SPIR-V via shaderc at build time and then runs the
// SPIR-V back through SPIRV-Cross's `CompilerGLSL` for reflection + GL-side
// decompile (see `OpenGLShader::CompileOrGetOpenGLBinaries`). SPIR-V binary
// parsing is a pure data-in / string-out transform with a large attack
// surface — historically source of several CVEs in Khronos' reference
// implementations. Driving the harness at `CompilerGLSL::compile()` means
// ASan / UBSan observe the parse, validation, and compile phases end-to-end.
//
// Inputs are treated as raw SPIR-V word streams. We pad to a 4-byte boundary
// because the library requires it; invalid magic / bad opcodes / truncated
// instructions are the interesting failure modes the fuzzer should find.
//
// Exceptions from SPIRV-Cross are expected (malformed SPIR-V throws
// `CompilerError`); the harness swallows them — a crash / UAF / OOB is the
// only thing that should terminate the process.
// =============================================================================

#include <spirv_cross/spirv_glsl.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Cap at 1 MiB of SPIR-V; the largest shader in the engine is well
    // below 64 KiB of SPIR-V, so 1 MiB is generous without triggering OOM.
    if (size < 4 || size > static_cast<size_t>(1 << 20))
        return 0;

    // Copy + pad to a multiple of 4 bytes so `vector<uint32_t>` construction
    // never reads past the tail of `data`.
    const size_t padded = (size + 3u) & ~static_cast<size_t>(3u);
    std::vector<uint32_t> words(padded / 4, 0u);
    std::memcpy(words.data(), data, size);

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
        // Malformed SPIR-V routinely raises `spirv_cross::CompilerError`
        // (derives from std::exception). That's the library's normal
        // error-reporting path — not a bug in the engine.
    }
    catch (...)
    {
        // Safety net: some exception types in SPIRV-Cross don't derive
        // from std::exception. Still not a crash.
    }

    return 0;
}

// OLO_TEST_LAYER: plumbing
//
// TWO SHARED DECISIONS THAT MUST NOT BE RE-IMPLEMENTED PER PATH (issue #629).
//
// Eleven confirmed divergences between the classic and virtualized-geometry mesh paths
// survived a 4593-test suite because every test compared a path against ITSELF. Two of
// those divergences were literally "someone re-implemented a shared decision inline and it
// drifted":
//
//   * VirtualVisibilityResolve.glsl hand-copied getNormalFromMap's tangent-frame
//     construction out of PBRCommon.glsl. The copy then drifted in TWO ways — it sampled
//     tangent-space z from the blue channel (re-introducing the #440 BC5/RGTC inversion:
//     a two-channel normal map has blue = 0 on the GPU, so z came out -1 and the normal
//     INVERTED) and it built the bitangent as +cross(N,T) instead of -cross(N,T). The
//     software rasterizer shades the MAJORITY of virtual pixels, so both halves of one
//     frame lit the same material differently.
//
//   * The classic MeshComponent loop resolved MaterialComponent -> engine default and never
//     consulted the submesh's IMPORTED material; Model::DrawParallel resolved imported ->
//     default and ignored an authored MaterialComponent. Each path implemented a different
//     HALF of the precedence rule, so a multi-material glTF through a MeshComponent shaded
//     every submesh flat grey.
//
// Both are now single-sourced (PBRCommon's decodeTangentNormal/applyNormalMapTBN;
// SubmeshMaterialResolve.h's ResolveSubmeshMaterial/MaterialCastsShadows) — but nothing
// STOPS the next path from copying the code again. A pixel test cannot: it can only fail
// after a copy has already drifted, and only if it happens to render the case that drifted.
// So this is a SOURCE-TEXT test, the idiom this repo already uses for its hand-maintained
// touch-points (ComponentSerializerCoverageTest, ComponentTupleCoverageTest,
// RendererShutdownTest). It is headless and runs on every PR, unlike the GPU tests that skip
// on CI.
//
// Both rules carry an EXPLICIT OPT-OUT marker, so a legitimately-different path is a
// one-line, reviewable, greppable declaration rather than a reason to delete the test:
//
//     // OLO_NORMAL_MAP_TBN_EXEMPT: <why this shader may build its own tangent frame>
//     // OLO_MATERIAL_RESOLVE_EXEMPT: <why this TU may submit geometry without resolving>
//
// TRAPS THIS TEST ALREADY WALKED INTO (see RendererShutdownTest's header — same list):
//   * A source scan matches code that has merely been COMMENTED OUT, so it passes against
//     the very bug it exists to catch. Comments are stripped before matching.
//   * A relative path passes under --gtest_filter and fails in a full run, because another
//     test in this binary chdir()s. The cwd is captured at STATIC INIT and walked up.

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The working directory at PROCESS START. Another test in this binary chdir()s into
        // OloEditor/ (RenderPropertyFixture does it so shaders resolve), so a relative path
        // resolved inside a test body depends on which tests ran first.
        const std::filesystem::path s_StartCwd = std::filesystem::current_path();

        // Walk up from the start cwd until the marker file appears.
        [[nodiscard]] std::filesystem::path RepoRoot()
        {
            std::error_code ec;
            for (std::filesystem::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (std::filesystem::exists(dir / "OloEngine/src/OloEngine/Renderer/SubmeshMaterialResolve.h", ec))
                {
                    return dir;
                }
                if (!dir.has_relative_path())
                {
                    break;
                }
            }
            return s_StartCwd;
        }

        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path);
            if (!in.is_open())
            {
                return {};
            }
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Drop // line comments and /* */ block comments. Without this, a shared call that has
        // merely been COMMENTED OUT still satisfies the "does it call the shared helper?" search,
        // and an example of a hand-rolled TBN quoted in a comment counts as a hand-rolled TBN.
        [[nodiscard]] std::string StripComments(const std::string& source)
        {
            std::string out;
            out.reserve(source.size());
            bool inBlock = false;
            std::istringstream in(source);
            std::string line;
            while (std::getline(in, line))
            {
                std::string kept;
                for (sizet i = 0; i < line.size(); ++i)
                {
                    if (inBlock)
                    {
                        if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/')
                        {
                            inBlock = false;
                            ++i;
                        }
                        continue;
                    }
                    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
                    {
                        break; // rest of the line is a comment
                    }
                    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*')
                    {
                        inBlock = true;
                        ++i;
                        continue;
                    }
                    kept += line[i];
                }
                out += kept;
                out += '\n';
            }
            return out;
        }

        // Files, sorted, so a failure names the same file every run.
        [[nodiscard]] std::vector<std::filesystem::path> FilesUnder(const std::filesystem::path& root,
                                                                    const std::vector<std::string>& extensions)
        {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            if (!std::filesystem::exists(root, ec))
            {
                return files;
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const std::string ext = entry.path().extension().string();
                if (std::ranges::find(extensions, ext) != extensions.end())
                {
                    files.push_back(entry.path());
                }
            }
            std::ranges::sort(files);
            return files;
        }

        [[nodiscard]] std::string Relative(const std::filesystem::path& path, const std::filesystem::path& root)
        {
            std::error_code ec;
            std::filesystem::path const rel = std::filesystem::relative(path, root, ec);
            return ec ? path.string() : rel.generic_string();
        }
    } // namespace

    // -------------------------------------------------------------------------------------
    // (a) ONE tangent frame. A shader that samples a normal map goes through PBRCommon.
    //
    // This is the shape of the bug of 3cb14987, stated as a rule: a shader that binds a
    // normal-map sampler must call getNormalFromMap / getNormalFromMapGrad and must NOT build
    // a tangent frame of its own.
    // -------------------------------------------------------------------------------------
    TEST(RenderPathDrift, EveryNormalMappedShaderUsesPBRCommonsTangentFrame)
    {
        const std::filesystem::path root = RepoRoot();
        const std::filesystem::path shaderDir = root / "OloEditor/assets/shaders";
        ASSERT_TRUE(std::filesystem::exists(shaderDir))
            << "shader directory not found from cwd " << s_StartCwd.string() << " — repo root resolved to "
            << root.string();

        // PBRCommon.glsl IS the single source of truth: it is the one file allowed to contain
        // the TBN construction.
        const std::filesystem::path owner = shaderDir / "include/PBRCommon.glsl";

        // The MATERIAL normal-map slot. Every shader that shades an imported mesh binds it
        // under this name (u_NormalMap; water's detail-wave maps are u_NormalMap0/1). A
        // sampler of SCREEN-SPACE normals (u_GBufferNormal, u_ViewNormals, …) is a normal
        // CONSUMER, not a normal-mapper, and is deliberately not matched.
        const std::regex materialSlotRe(R"(uniform\s+sampler2D\s+u_NormalMap\w*)");
        // Goes through the shared implementation.
        const std::regex sharedCallRe(R"(getNormalFromMap(Grad)?\s*\()");
        // Builds a tangent frame of its own. A hand-copied getNormalFromMap always contains
        // one of these — that IS what it is — so this is the pattern that catches the copy
        // before it has a chance to drift, whatever the copier calls their samplers.
        const std::regex handRolledTbnRe(
            R"(mat3\s*\(\s*T\s*,\s*B\s*,\s*N\s*\)|cross\s*\(\s*N\s*,\s*T\s*\)|cross\s*\(\s*T\s*,\s*N\s*\))");
        const std::regex exemptRe(R"(OLO_NORMAL_MAP_TBN_EXEMPT\s*:\s*\S)");

        std::vector<std::string> offenders;
        u32 materialShaders = 0;
        u32 scanned = 0;

        for (const std::filesystem::path& file : FilesUnder(shaderDir, { ".glsl", ".comp", ".vert", ".frag" }))
        {
            if (std::filesystem::equivalent(file, owner))
            {
                continue;
            }
            ++scanned;

            const std::string raw = ReadFile(file);
            const std::string source = StripComments(raw);
            const std::string name = Relative(file, root);

            const bool bindsMaterialSlot = std::regex_search(source, materialSlotRe);
            const bool handRolled = std::regex_search(source, handRolledTbnRe);
            if (!bindsMaterialSlot && !handRolled)
            {
                continue;
            }
            materialShaders += bindsMaterialSlot ? 1u : 0u;

            // The exemption marker is read from the RAW text (it lives in a comment).
            if (std::regex_search(raw, exemptRe))
            {
                continue;
            }

            if (bindsMaterialSlot && !std::regex_search(source, sharedCallRe))
            {
                offenders.push_back(name + ": binds the material normal-map slot but never calls getNormalFromMap / "
                                           "getNormalFromMapGrad");
            }
            if (handRolled)
            {
                offenders.push_back(name + ": builds its OWN tangent frame (mat3(T,B,N) / cross(N,T)) — this is the "
                                           "exact drift that re-introduced the #440 BC5 inversion and a flipped "
                                           "bitangent in the software rasterizer");
            }
        }

        EXPECT_GT(scanned, 20u) << "the shader scan found almost no files — the path or the extension list is wrong "
                                   "and this test is vacuous";
        EXPECT_GE(materialShaders, 5u)
            << "the material normal-map slot scan matched only " << materialShaders
            << " shaders — the naming convention has drifted from the regex and this test is vacuous";

        std::string report;
        for (const std::string& offender : offenders)
        {
            report += "\n  * " + offender;
        }
        EXPECT_TRUE(offenders.empty())
            << "these shaders do not share PBRCommon's ONE normal-map decode + ONE tangent frame:" << report
            << "\n\nRoute them through getNormalFromMap (hardware raster: dFdx/dFdy available) or "
               "getNormalFromMapGrad (a fullscreen resolve, which must supply its derivatives analytically).\n"
               "If the path is legitimately different — a procedural/analytic frame that is not a material normal "
               "map — say so in the shader:\n"
               "    // OLO_NORMAL_MAP_TBN_EXEMPT: <reason>";
    }

    // -------------------------------------------------------------------------------------
    // (b) ONE material resolver. A TU that submits mesh geometry resolves through
    //     SubmeshMaterialResolve.h — it does not open-code
    //     `override ? ... : imported ? ... : default`.
    //
    // The 6 call sites today are Model.cpp, Renderer3DMeshSubmission.cpp, Scene.cpp (x3-4)
    // and McpToolsRender.cpp. Nothing stops a NEW submission path — a skinned path, an
    // instanced path, a future component — from re-implementing the precedence inline, which
    // is exactly what the classic paths did (each got a different half of it right).
    // -------------------------------------------------------------------------------------
    TEST(RenderPathDrift, EveryMeshSubmissionPathUsesTheSharedMaterialResolver)
    {
        const std::filesystem::path root = RepoRoot();
        ASSERT_TRUE(std::filesystem::exists(root / "OloEngine/src")) << "repo root not found from " << s_StartCwd.string();

        // A CALL to one of the mesh-submission entry points. (The `Renderer3D::` /  `.`-free
        // spelling also matches the definitions inside Renderer3DMeshSubmission.cpp, which is
        // fine — that TU is a resolver itself.)
        const std::regex submissionRe(R"((SubmitMeshesParallel|SubmitVirtualMesh|DrawMeshInstanced)\s*\()");
        const std::regex resolverRe(R"(ResolveSubmeshMaterial\s*\(|MaterialCastsShadows\s*\()");
        const std::regex exemptRe(R"(OLO_MATERIAL_RESOLVE_EXEMPT\s*:\s*\S)");

        // The material precedence, open-coded. Both spellings the two classic paths used.
        const std::regex importedAccessRe(R"(GetImportedMaterialForSubmesh\s*\()");

        std::vector<std::string> offenders;
        std::vector<std::string> submissionTUs;

        for (const std::filesystem::path& dir : { root / "OloEngine/src", root / "OloEditor/src" })
        {
            for (const std::filesystem::path& file : FilesUnder(dir, { ".cpp" }))
            {
                const std::string raw = ReadFile(file);
                const std::string source = StripComments(raw);

                const bool submits = std::regex_search(source, submissionRe);
                const bool readsImported = std::regex_search(source, importedAccessRe);
                if (!submits && !readsImported)
                {
                    continue;
                }

                const std::string name = Relative(file, root);
                submissionTUs.push_back(name);

                if (std::regex_search(raw, exemptRe))
                {
                    continue; // declared, reviewable opt-out
                }
                if (!std::regex_search(source, resolverRe))
                {
                    offenders.push_back(name);
                }
            }
        }

        EXPECT_GE(submissionTUs.size(), 3u)
            << "the mesh-submission scan matched fewer TUs than exist — the regex has drifted from the code and this "
               "test is vacuous";

        std::string report;
        for (const std::string& offender : offenders)
        {
            report += "\n  * " + offender;
        }
        EXPECT_TRUE(offenders.empty())
            << "these translation units submit mesh geometry (or read a submesh's imported material) without going "
               "through the shared resolver:"
            << report
            << "\n\nUse ResolveSubmeshMaterial / MaterialCastsShadows from "
               "OloEngine/src/OloEngine/Renderer/SubmeshMaterialResolve.h. Open-coding "
               "`override ? ... : imported ? ... : default` is how the classic and virtualized paths ended up "
               "implementing DIFFERENT HALVES of the rule (a multi-material glTF through a MeshComponent shaded "
               "every submesh flat grey), and how alpha-masked foliage ended up casting solid shadows (the gate "
               "asked the ENTITY's MaterialComponent, not the RESOLVED material).\n"
               "If the path legitimately does not resolve a per-submesh material — a packet dispatcher, a debug "
               "draw that is handed its material — say so in the TU:\n"
               "    // OLO_MATERIAL_RESOLVE_EXEMPT: <reason>";
    }
} // namespace OloEngine::Tests
